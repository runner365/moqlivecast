import { log_error, log_info, log_warn } from '../logger'
import { AudioPlayer } from './audioPlayer'
import type { MediaStats } from './mediaStats'
import { VideoRenderer } from './videoRenderer'

/**
 * WebCodecs 播放器（编排器）。
 *
 * 输入约定（由服务端格式决定，**无需任何转换**）：
 *  - 视频 payload = AVCC（4 字节长度前缀 NALU）；avcC = AVCDecoderConfigurationRecord
 *  - 音频 payload = raw AAC（无 ADTS）；asc = AudioSpecificConfig
 *  - 时间戳单位：**微秒**（调用方已从毫秒 *1000）
 *
 * 音频（AudioDecoder + AudioWorklet）待第 5 步接入。
 */
export type WebCodecsPlayerOptions = {
  canvas: HTMLCanvasElement
  stats: MediaStats
}

/** 送进解码器的在途上限：解码队列低于它才继续喂下一帧。
 *  它同时是"解码输出在途帧数"的上限 —— 所以是**内存**的约束（1080p 一帧
 *  NV12 约 3MB，12 帧 ≈ 36MB），不是延迟的约束。 */
const MAX_DECODE_QUEUE = 12
/** 待解码积压上限（已入队、还没送进解码器的编码块）。
 *  编码块很小（几 KB），可以放得比 MAX_DECODE_QUEUE 宽得多。
 *  它只用来判"解码器真的长期跟不上"（约 16 秒视频），不是用来削突发的。 */
const MAX_PENDING_VIDEO = 400

/* ── 音频丢帧（pts 空洞）的检测/填补 ──
 * 服务端实测丢 3.4% 音频帧（源文件本身一帧不缺、视频一帧不丢），
 * 表现为编码块 pts 上的 ~21ms 空洞。不处理的话音频内容会比媒体时间轴跑得快，
 * 而视频 slave 在音频时钟上 → 音画差距每秒长 ~4%，单调增大。 */
/** 判定为"丢帧"的最小空洞（微秒）：小于它当网络抖动。一帧 AAC ≈ 21.3ms。 */
const AUDIO_GAP_MIN_US = 5_000
/** 单次补静音上限（微秒）：超过它说明是时间轴断点（服务端重锚/重启），不是丢帧 */
const AUDIO_GAP_MAX_US = 200_000
/** 视频解码参数变化的兜底重建阈值 */
const MAX_HW_FAILS = 3

export class WebCodecsPlayer {
  private readonly canvas: HTMLCanvasElement
  private readonly stats: MediaStats
  private renderer: VideoRenderer | null = null
  private closed = false

  private avcC: Uint8Array | null = null
  private asc: Uint8Array | null = null

  private videoDecoder: VideoDecoder | null = null
  private videoCodec = ''
  /** 丢过帧之后要等到下一个关键帧才能继续喂（否则解码器会花屏） */
  private needKey = false
  private videoCount = 0
  private audioCount = 0
  private decodeErrors = 0
  private outOfOrder = 0
  private lastPtsUs = -1
  private decodedFrames = 0
  private needKeyDrops = 0
  private queueDrops = 0
  private firstFrameLogged = false

  /* ── 视频解码削峰 ──
   * 加入订阅时服务端会一次性回放一整个 GOP（实测 96 帧），这些对象是在
   * **同一个 tick 里**读完并交给 pushVideo 的。若在这里直接 decode()，
   * decodeQueueSize 会瞬间冲过 MAX_DECODE_QUEUE，触发"丢非关键帧 + 等关键帧"
   * 的兜底 —— 而下一个关键帧在一整个 GOP 之后（≈3.8s），于是画面冻住约 4 秒
   * （实测 rendered 从 2 卡到 03.091→06.108 才恢复）。
   * 改成先把编码块排进 pendingVideo，按解码器剩余容量逐帧喂：
   * 在途帧数始终 ≤ MAX_DECODE_QUEUE，突发被摊成一段时间的解码；
   * 解出来但已经过时的帧由 VideoRenderer 按媒体时钟丢弃（它本来就有这条逻辑）。 */
  private pendingVideo: { payload: Uint8Array; ptsUs: number; key: boolean }[] = []
  /** 已送进解码器的块数，只用于日志节奏 */
  private fedVideo = 0

  /* ── 音频 ── */
  private audioPlayer: AudioPlayer | null = null
  private audioDecoder: AudioDecoder | null = null
  private audioCodec = ''
  private audioSampleRate = 48000
  private audioChannels = 2
  private decodedAudioFrames = 0
  private audioDecodeErrors = 0
  private audioClockReady = false
  /** 每个已提交、还没拿到 PCM 的编码块各自要在前面补的静音时长（微秒）。
   *  AAC 一 chunk 出一块 PCM、次序一致，所以用下标对齐即可（长度 = 解码队列深度）。 */
  private audioPadQueue: number[] = []
  /** 上一个已提交编码块的媒体时间终点（微秒）；-1 = 未建立连续性（首块/刚 flush） */
  private audioChunkEndUs = -1
  /** 实测每 chunk 解出的帧数（AAC-LC = 1024），首个 PCM 之后按实测校准 */
  private audioFramesPerChunk = 1024
  private audioGapCount = 0

  constructor(opts: WebCodecsPlayerOptions) {
    this.canvas = opts.canvas
    this.stats = opts.stats
  }

  /**
   * 必须在用户手势的同一任务里同步调用（不能 await 之后再建 AudioContext，
   * 否则 autoplay 策略会把它挂起）。moqPuller.start() 首行调用。
   */
  warmup() {
    this.renderer = new VideoRenderer(this.canvas)
    this.renderer.start()
    /* 先画测试图案：验证 GL 通路，同时避免"等待数据"时黑屏。
     * 第一帧真实视频到达后会被覆盖。 */
    void this.renderer.renderTestPattern()

    /* 音频默认 48k/2ch 起 AudioContext（真实参数在 ASC 到达后协商）；
     * worklet 模块异步加载，失败则降级为纯视频。 */
    this.audioPlayer = new AudioPlayer()
    this.audioPlayer.onTelemetry = (t) => {
      if (!this.audioClockReady && t.consumedFrames > 0) {
        this.audioClockReady = true
        log_info('wc-audio', `clock active (consumed=${t.consumedFrames})`)
      }
    }
    this.audioPlayer.warmup(this.audioSampleRate, this.audioChannels)
    void this.initAudioWorklet()

    log_info('wc-player', `warmup renderer_ready=${this.renderer.ready}`)
  }

  private async initAudioWorklet() {
    const ap = this.audioPlayer
    if (!ap) return
    const ok = await ap.init()
    if (ok) {
      /* 音频时钟可用后，视频跟着音频走（比墙钟更准，且音画天然同步）。
       * 返回 -1（未起播/欠载未开始）时 renderer 会回退到自己的锚定时钟。 */
      const renderer = this.renderer
      if (renderer) {
        renderer.setClock(() => {
          const t = ap.mediaTimeUs()
          return t /* -1 = 不可用，renderer 内部会退回锚定时钟 */
        })
      }
      log_info('wc-player', 'audio clock wired to renderer')
    } else {
      log_warn('wc-player', 'audio worklet unavailable, video-only playback')
    }
  }

  /** avcC 原始字节 → 推导 codec 字符串并建 VideoDecoder */
  configureVideo(avcC: Uint8Array) {
    log_info('wc-player', `configureVideo ENTER avcC=${avcC.byteLength}B hex=${hex(avcC, 8)}`)
    this.avcC = avcC.slice()
    const codec = codecFromAvcC(avcC)
    if (!codec) {
      log_error('wc-player', `configureVideo: bad avcC len=${avcC.byteLength}`)
      return
    }
    this.videoCodec = codec
    this.buildDecoder()
    /* 配置就绪 → 下一帧开始解码；若之前因丢帧等关键帧，这里保持 needKey 不动 */
    log_info(
      'wc-player',
      `configureVideo codec=${codec} avcC=${avcC.byteLength}B hex=${hex(avcC, 8)}`,
    )
  }

  private buildDecoder() {
    const avcC = this.avcC
    if (!avcC || !this.videoCodec) return
    try {
      this.videoDecoder?.close()
    } catch {
      /* ignore */
    }
    const cfg: VideoDecoderConfig = {
      codec: this.videoCodec,
      description: avcC.slice(),
      optimizeForLatency: true,
    }
    const dec = new VideoDecoder({
      output: (frame) => this.onVideoFrame(frame),
      error: (e) => this.onVideoError(e),
    })
    try {
      dec.configure(cfg)
    } catch (e) {
      log_error(
        'wc-player',
        `VideoDecoder.configure FAILED: ${errMsg(e)} codec=${this.videoCodec} ` +
          `avcC=${avcC.byteLength}B`,
      )
      try {
        dec.close()
      } catch {
        /* ignore */
      }
      this.videoDecoder = null
      return
    }
    this.videoDecoder = dec
    this.needKey = true /* 新 decoder 必须从关键帧起 */
    /* 解码队列变浅就继续喂 pendingVideo —— 突发靠这个自己消化，不需要外部推动。
     * 另外 onVideoFrame 里也补一次：ondequeue 不是所有实现都可靠。 */
    dec.ondequeue = () => this.drainVideo()
    /* 旧配置的编码块作废（同软件回退那处） */
    this.pendingVideo.length = 0
    log_info(
      'wc-player',
      `VideoDecoder configured codec=${this.videoCodec} avcC=${avcC.byteLength}B hex=${hex(avcC, 6)}`,
    )
  }

  private onVideoFrame(frame: VideoFrame) {
    if (this.closed || !this.renderer) {
      log_error('wc-player', `frame dropped: closed=${this.closed} renderer=${!!this.renderer}`)
      frame.close()
      return
    }
    this.decodedFrames++
    if (!this.firstFrameLogged) {
      this.firstFrameLogged = true
      log_info(
        'wc-player',
        `first frame decoded ts=${frame.timestamp}us ${frame.displayWidth}x${frame.displayHeight}`,
      )
    }
    if (frame.timestamp <= this.lastPtsUs) {
      this.outOfOrder++
      if (this.outOfOrder <= 3 || this.outOfOrder % 60 === 0) {
        log_warn(
          'wc-player',
          `frame out-of-order ts=${frame.timestamp} last=${this.lastPtsUs} (#${this.outOfOrder})`,
        )
      }
    }
    this.lastPtsUs = Math.max(this.lastPtsUs, frame.timestamp)
    this.renderer.push(frame.timestamp, frame)
    /* 吐出一帧 → 解码队列浅了一格 → 继续喂。突发靠这里推进，
     * 不依赖 ondequeue（两者都调是幂等的：容量满了就直接返回）。 */
    this.drainVideo()
  }

  private onVideoError(e: DOMException) {
    this.decodeErrors++
    log_error('wc-player', `VideoDecoder error: ${e.message} (#${this.decodeErrors})`)
    /* 重建 decoder；needKey 由 buildDecoder 置位，等下一个关键帧恢复 */
    if (this.decodeErrors >= MAX_HW_FAILS && this.videoCodec) {
      /* 多次失败：退一步试 software */
      try {
        this.videoDecoder?.close()
      } catch {
        /* ignore */
      }
      this.videoDecoder = null
      const soft: VideoDecoderConfig = {
        codec: this.videoCodec,
        description: this.avcC?.slice(),
        optimizeForLatency: true,
        hardwareAcceleration: 'prefer-software',
      }
      try {
        const dec = new VideoDecoder({
          output: (f) => this.onVideoFrame(f),
          error: (e2) => {
            this.decodeErrors++
            log_error('wc-player', `VideoDecoder(sw) error: ${e2.message}`)
          },
        })
        dec.configure(soft)
        dec.ondequeue = () => this.drainVideo()
        this.videoDecoder = dec
        this.needKey = true
        /* 旧配置的编码块作废：新 decoder 要从关键帧起，排队中的那些不能直接续上 */
        this.pendingVideo.length = 0
        log_warn('wc-player', 'VideoDecoder rebuilt as prefer-software')
      } catch (e2) {
        log_error('wc-player', `software fallback failed: ${errMsg(e2)}`)
        this.videoDecoder = null
      }
    } else {
      this.buildDecoder()
    }
  }

  /** ptsUs 单位微秒；key = 是否 IDR */
  pushVideo(payload: Uint8Array, ptsUs: number, key: boolean) {
    if (this.closed || payload.byteLength === 0) return
    this.videoCount++
    const dec = this.videoDecoder
    if (!dec || dec.state === 'closed') {
      log_error(
        'wc-player',
        `pushVideo dropped: decoder=${dec ? dec.state : 'null'} configured=${this.avcConfigured()}`,
      )
      return
    }

    /* 丢帧策略：丢过帧之后必须从关键帧才能继续（否则解码器会花屏） */
    if (this.needKey && !key) {
      this.needKeyDrops++
      if (this.needKeyDrops <= 3 || this.needKeyDrops % 60 === 0) {
        log_warn('wc-player', `wait-keyframe: drop delta (${this.needKeyDrops})`)
      }
      return
    }

    /* 积压过深 = 解码器长期跟不上（不是回放突发 —— 突发由 pending 队列自然消化）。
     * 非关键帧丢掉并等下一个关键帧；关键帧来了就整队清掉，从它重新开始。 */
    if (this.pendingVideo.length > MAX_PENDING_VIDEO) {
      if (!key) {
        this.needKey = true
        this.queueDrops++
        if (this.queueDrops <= 3 || this.queueDrops % 60 === 0) {
          log_warn(
            'wc-player',
            `backpressure: pending=${this.pendingVideo.length} > ${MAX_PENDING_VIDEO}, drop delta (#${this.queueDrops})`,
          )
        }
        return
      }
      log_warn(
        'wc-player',
        `backpressure: pending=${this.pendingVideo.length} > ${MAX_PENDING_VIDEO}, drop backlog on key (#${this.queueDrops})`,
      )
      this.pendingVideo.length = 0
    }
    if (key) this.needKey = false

    this.pendingVideo.push({ payload, ptsUs, key })
    this.drainVideo()
  }

  /**
   * 按解码器剩余容量把 pendingVideo 喂进去。**每次解码器吐出一帧、或队列变浅时都会被调用**
   * （onVideoFrame / ondequeue），所以突发不会卡在队列里等人推。
   */
  private drainVideo() {
    const dec = this.videoDecoder
    if (this.closed || !dec || dec.state === 'closed') {
      this.pendingVideo.length = 0
      return
    }
    while (this.pendingVideo.length > 0 && dec.decodeQueueSize < MAX_DECODE_QUEUE) {
      const item = this.pendingVideo.shift()
      if (!item) break
      try {
        dec.decode(
          new EncodedVideoChunk({
            type: item.key ? 'key' : 'delta',
            timestamp: item.ptsUs,
            data: item.payload,
          }),
        )
        this.fedVideo++
        if (this.fedVideo <= 3 || this.fedVideo % 120 === 0) {
          log_info(
            'wc-player',
            `pushVideo #${this.videoCount} #fed${this.fedVideo} ${item.payload.byteLength}B ` +
              `ptsUs=${item.ptsUs} key=${item.key ? 1 : 0} ` +
              `q=${dec.decodeQueueSize} pend=${this.pendingVideo.length} frames=${this.decodedFrames}`,
          )
        }
      } catch (e) {
        log_error(
          'wc-player',
          `decode() threw: ${errMsg(e)} ptsUs=${item.ptsUs} key=${item.key ? 1 : 0}`,
        )
      }
    }
  }

  private avcConfigured(): boolean {
    return !!this.avcC
  }

  /** ASC 原始字节 → 解出 codec/采样率/声道数并建 AudioDecoder */
  configureAudio(asc: Uint8Array) {
    this.asc = asc.slice()
    const info = parseAsc(asc)
    if (!info) {
      log_error('wc-player', `configureAudio: bad ASC len=${asc.byteLength} hex=${hex(asc, 4)}`)
      return
    }
    this.audioCodec = `mp4a.40.${info.objectType}`
    this.audioSampleRate = info.sampleRate
    this.audioChannels = info.channels
    this.buildAudioDecoder()
    log_info(
      'wc-player',
      `configureAudio codec=${this.audioCodec} sr=${this.audioSampleRate} ch=${this.audioChannels} ` +
        `asc=${asc.byteLength}B hex=${hex(asc, 4)}`,
    )
  }

  private buildAudioDecoder() {
    const asc = this.asc
    if (!asc || !this.audioCodec) return
    try {
      this.audioDecoder?.close()
    } catch {
      /* ignore */
    }
    const dec = new AudioDecoder({
      output: (data) => this.onAudioData(data),
      error: (e) => this.onAudioError(e),
    })
    try {
      dec.configure({
        codec: this.audioCodec,
        sampleRate: this.audioSampleRate,
        numberOfChannels: this.audioChannels,
        description: asc.slice(),
      })
    } catch (e) {
      log_error('wc-player', `AudioDecoder.configure FAILED: ${errMsg(e)} codec=${this.audioCodec}`)
      try {
        dec.close()
      } catch {
        /* ignore */
      }
      this.audioDecoder = null
      return
    }
    this.audioDecoder = dec
    /* 新 decoder 会重置输出 → 清掉环形缓冲，避免旧数据造成错位 */
    this.audioPlayer?.flush()
    /* 时间轴连续性锚点与待补空洞都要清掉：新 decoder 的输出是另一条序列，
     * 旧队列里的 pad 会对不上，而残留的 chunk 终点会被误判成一个巨大空洞。 */
    this.audioPadQueue.length = 0
    this.audioChunkEndUs = -1
    log_info('wc-player', `AudioDecoder configured codec=${this.audioCodec}`)
  }

  private onAudioData(data: AudioData) {
    const ap = this.audioPlayer
    if (this.closed || !ap) {
      data.close()
      return
    }
    this.decodedAudioFrames += data.numberOfFrames
    if (this.decodedAudioFrames <= data.numberOfFrames) {
      log_info(
        'wc-audio',
        `first PCM ${data.numberOfFrames}f ${data.numberOfChannels}ch sr=${data.sampleRate} ` +
          `ts=${data.timestamp}us`,
      )
    }
    /* 按实测校准"每 chunk 多少帧"（用于推算 chunk 的媒体时长） */
    if (data.numberOfFrames > 0) this.audioFramesPerChunk = data.numberOfFrames
    /* 这个 chunk 提交时检测到的 pts 空洞，在这里把静音补在它前面 ——
     * 必须在这里（而不是 pushAudio 那一刻），否则解码队列里积压的
     * 前几块 PCM 会被排到静音后面，静音就落错了位置。 */
    const padUs = this.audioPadQueue.shift() ?? 0
    if (padUs > 0) ap.padSilenceUs(padUs)
    try {
      /* f32-planar → 交错 Float32，喂给 worklet */
      const ch = data.numberOfChannels
      const frames = data.numberOfFrames
      const planes: Float32Array[] = []
      for (let c = 0; c < ch; c++) {
        const p = new Float32Array(frames)
        data.copyTo(p, { planeIndex: c, format: 'f32-planar' })
        planes.push(p)
      }
      const interleaved = new Float32Array(frames * ch)
      for (let i = 0; i < frames; i++) {
        for (let c = 0; c < ch; c++) {
          interleaved[i * ch + c] = planes[c][i]
        }
      }
      ap.push(interleaved, data.timestamp)
    } catch (e) {
      log_error('wc-audio', `AudioData copy failed: ${errMsg(e)}`)
    } finally {
      data.close()
    }
  }

  private onAudioError(e: DOMException) {
    this.audioDecodeErrors++
    log_error('wc-audio', `AudioDecoder error: ${e.message} (#${this.audioDecodeErrors})`)
    this.buildAudioDecoder()
  }

  pushAudio(payload: Uint8Array, ptsUs: number) {
    if (this.closed || payload.byteLength === 0) return
    this.audioCount++
    const dec = this.audioDecoder
    if (!dec || dec.state === 'closed') {
      log_error('wc-player', `pushAudio dropped: decoder=${dec ? dec.state : 'null'}`)
      return
    }
    /* 音频丢帧**唯一可见的地方**就是这里：编码块 pts 上的空洞。
     * 解码后的 AudioData.timestamp 实测是连续的（audioPlayer 的 decGapMax 恒为 0），
     * 所以在那一层做空洞检测永远不会命中 —— 这是上一版补静音没生效的原因。
     * 检测到的空洞排队，等对应的 PCM 出来时在它前面注入等长静音，
     * 让写入序列与媒体时间轴对齐（见 AudioPlayer.padSilenceUs）。 */
    const nominalUs = (this.audioFramesPerChunk / this.audioSampleRate) * 1e6
    let padUs = 0
    if (this.audioChunkEndUs >= 0) {
      const g = ptsUs - this.audioChunkEndUs
      if (g > AUDIO_GAP_MIN_US) {
        padUs = Math.min(g, AUDIO_GAP_MAX_US)
        this.audioGapCount++
        if (this.audioGapCount <= 3 || this.audioGapCount % 200 === 0) {
          log_warn(
            'wc-player',
            `audio chunk pts gap ${(g / 1000).toFixed(1)}ms at ptsUs=${ptsUs} ` +
              `(#${this.audioGapCount}) -> will pad silence`,
          )
        }
      }
    }
    this.audioChunkEndUs = ptsUs + nominalUs
    this.audioPadQueue.push(padUs)
    try {
      dec.decode(
        new EncodedAudioChunk({
          type: 'key', /* AAC 每帧都是独立可解单元 */
          timestamp: ptsUs,
          data: payload,
        }),
      )
      if (this.audioCount <= 3 || this.audioCount % 500 === 0) {
        log_info(
          'wc-player',
          `pushAudio #${this.audioCount} ${payload.byteLength}B ptsUs=${ptsUs} ` +
            `q=${dec.decodeQueueSize} frames=${this.decodedAudioFrames}`,
        )
      }
    } catch (e) {
      log_error('wc-player', `audio decode() threw: ${errMsg(e)}`)
    }
  }

  close() {
    if (this.closed) return
    this.closed = true
    const v = this.videoCount
    const a = this.audioCount
    const rs = this.renderer?.getStats()
    const at = this.audioPlayer?.getTelemetry()
    this.videoCount = 0
    this.audioCount = 0
    this.avcC = null
    this.asc = null
    try {
      this.videoDecoder?.close()
    } catch {
      /* ignore */
    }
    this.videoDecoder = null
    try {
      this.audioDecoder?.close()
    } catch {
      /* ignore */
    }
    this.audioDecoder = null
    this.renderer?.close()
    this.renderer = null
    this.audioPlayer?.close()
    this.audioPlayer = null
    log_info(
      'wc-player',
      `close pushV=${v} decoded=${this.decodedFrames} decodeErr=${this.decodeErrors} ` +
        `outOfOrder=${this.outOfOrder} waitKeyDrop=${this.needKeyDrops} queueDrop=${this.queueDrops} ` +
        `pushA=${a} decodedA=${this.decodedAudioFrames} audioErr=${this.audioDecodeErrors} ` +
        `audioTelem=${JSON.stringify(at)} render=${JSON.stringify(rs)}`,
    )
  }
}

/** avcC 的字节 1/2/3 = profile_idc / profile_compatibility / level_idc */
function codecFromAvcC(avcC: Uint8Array): string | null {
  if (avcC.byteLength < 4 || avcC[0] !== 1) return null
  const b = (n: number) => n.toString(16).padStart(2, '0')
  return `avc1.${b(avcC[1])}${b(avcC[2])}${b(avcC[3])}`
}

/** AAC 采样率表（AudioSpecificConfig 的 samplingFrequencyIndex） */
const AAC_FREQ = [
  96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350,
]

/**
 * 解 AudioSpecificConfig（bit-packed）。
 * audioObjectType = asc[0]>>3；samplingFrequencyIndex = (asc[0]&0x7)<<1 | asc[1]>>7；
 * channelConfiguration = (asc[1]>>3)&0xf。
 */
function parseAsc(
  asc: Uint8Array,
): { objectType: number; sampleRate: number; channels: number } | null {
  if (asc.byteLength < 2) return null
  const objectType = (asc[0] >> 3) & 0x1f
  const freqIdx = ((asc[0] & 0x07) << 1) | (asc[1] >> 7)
  let channels = (asc[1] >> 3) & 0x0f

  /* 转义：objectType==31 时后续 6 bit 是真实值（罕见，不展开处理） */
  if (objectType === 31) return null

  let sampleRate: number
  if (freqIdx === 15) {
    /* 显式 24bit 采样率：跳过 objectType(5) 后的 4bit index + 24bit rate */
    if (asc.byteLength < 5) return null
    sampleRate =
      ((asc[1] & 0x7f) << 17) | (asc[2] << 9) | (asc[3] << 1) | ((asc[4] >> 7) & 0x01)
    channels = (asc[4] >> 3) & 0x0f
  } else {
    sampleRate = AAC_FREQ[freqIdx] ?? 48000
  }
  if (channels <= 0) channels = 2
  return { objectType, sampleRate, channels }
}

function errMsg(e: unknown): string {
  return e instanceof Error ? e.message : String(e)
}

function hex(buf: Uint8Array, n: number): string {
  let s = ''
  for (let i = 0; i < Math.min(n, buf.byteLength); i++) s += buf[i].toString(16).padStart(2, '0')
  return s
}
