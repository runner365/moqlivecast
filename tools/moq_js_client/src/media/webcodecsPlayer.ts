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

/** decodeQueueSize 超过它就开始丢非关键帧 */
const MAX_DECODE_QUEUE = 12
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

  /* ── 音频 ── */
  private audioPlayer: AudioPlayer | null = null
  private audioDecoder: AudioDecoder | null = null
  private audioCodec = ''
  private audioSampleRate = 48000
  private audioChannels = 2
  private decodedAudioFrames = 0
  private audioDecodeErrors = 0
  private audioClockReady = false

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
        this.videoDecoder = dec
        this.needKey = true
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

    /* 丢帧策略：队列过深时丢非关键帧，并进入"等关键帧"状态 */
    if (this.needKey && !key) {
      this.needKeyDrops++
      if (this.needKeyDrops <= 3 || this.needKeyDrops % 60 === 0) {
        log_warn('wc-player', `wait-keyframe: drop delta (${this.needKeyDrops})`)
      }
      return
    }
    if (dec.decodeQueueSize > MAX_DECODE_QUEUE) {
      if (!key) {
        this.needKey = true
        this.queueDrops++
        if (this.queueDrops <= 3 || this.queueDrops % 60 === 0) {
          log_warn(
            'wc-player',
            `backpressure: decodeQueueSize=${dec.decodeQueueSize} > ${MAX_DECODE_QUEUE}, drop delta (#${this.queueDrops})`,
          )
        }
        return
      }
      /* 关键帧来了：重建 decoder 清掉积压（reset 会清配置） */
      log_warn(
        'wc-player',
        `backpressure: decodeQueueSize=${dec.decodeQueueSize}, rebuild decoder on key`,
      )
      this.buildDecoder()
      if (!this.videoDecoder) return
    }
    if (key) this.needKey = false

    const target = this.videoDecoder
    if (!target || target.state === 'closed') return
    try {
      target.decode(
        new EncodedVideoChunk({
          type: key ? 'key' : 'delta',
          timestamp: ptsUs,
          data: payload,
        }),
      )
      if (this.videoCount <= 3 || this.videoCount % 120 === 0) {
        log_info(
          'wc-player',
          `pushVideo #${this.videoCount} ${payload.byteLength}B ptsUs=${ptsUs} key=${key ? 1 : 0} ` +
            `q=${target.decodeQueueSize} frames=${this.decodedFrames}`,
        )
      }
    } catch (e) {
      log_error('wc-player', `decode() threw: ${errMsg(e)} ptsUs=${ptsUs} key=${key ? 1 : 0}`)
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
