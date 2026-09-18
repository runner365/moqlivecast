import { hex_preview, log_debug, log_error, log_info, log_warn } from '../logger'
import type { AvSink } from './avSink'
import { aacLc48kStereoAsc } from './flvMuxer'

const SAMPLE_RATE = 48000
const CHANNELS = 2
const VIDEO_FPS = 15
const VIDEO_BITRATE = 800_000
const AUDIO_BITRATE = 128_000
const KEYFRAME_INTERVAL_US = 4_000_000

/* 实验：强制 H.264 High profile，不做降级。
 * 64=High, 00=constraint flags, 1f=Level 3.1（640x360@15fps 够用）。
 * 之前这里是 ['avc1.64001f','avc1.4d001f','avc1.42E01E']，会在
 * isConfigSupported 返回 false 时静默退到 Main/Baseline，
 * 导致「以为在测 High，实际跑的是别的档位」。 */
const VIDEO_CODEC = 'avc1.64001f'

/* latencyMode：直播推流取 'realtime'（低延迟优先）。
 *
 * 已实测（2026-09-18，Chrome + avc1.64001f @640x360）：改成 'quality'
 * 后浏览器确实接受了该配置（isConfigSupported 回填 latencyMode=quality），
 * 但 300 帧编码结果仍是「PTS 无回退」→ 不产生 B 帧。
 * 结论：WebCodecs 的 EncodedVideoChunk 只有一个时间戳字段，
 * 无法表达「解码顺序 ≠ 呈现顺序」，因此该路径天然不会有 B 帧，
 * 下游按 cts=0 组装是正确的。详见同文件的重排序检测日志。
 *
 * 注意：avc.format 必须保持 'avc'（AVCC）—— 下游服务端按 AVCC 拼 FLV，
 * 改成 'annexb' 会拿不到 avcC description，整条链路失效。 */
const VIDEO_LATENCY_MODE: LatencyMode = 'realtime'
const AUDIO_CODECS = ['mp4a.40.2', 'mp4a.40.02', 'aac']

export type EncoderStatus = {
  videoCodec: string
  audioCodec: string
  audioOk: boolean
  width: number
  height: number
}

function chunkBytes(chunk: EncodedVideoChunk | EncodedAudioChunk): Uint8Array {
  const buf = new Uint8Array(chunk.byteLength)
  chunk.copyTo(buf)
  return buf
}

function asU8(src: AllowSharedBufferSource): Uint8Array {
  if (ArrayBuffer.isView(src)) {
    return new Uint8Array(src.buffer, src.byteOffset, src.byteLength)
  }
  return new Uint8Array(src)
}

async function pickVideoCodec(width: number, height: number): Promise<VideoEncoderConfig> {
  const config: VideoEncoderConfig = {
    codec: VIDEO_CODEC,
    width,
    height,
    bitrate: VIDEO_BITRATE,
    framerate: VIDEO_FPS,
    latencyMode: VIDEO_LATENCY_MODE,
    avc: { format: 'avc' },
  }
  const r = await VideoEncoder.isConfigSupported(config)
  log_info(
    'encoder',
    `probe ${VIDEO_CODEC} ${width}x${height} latencyMode=${VIDEO_LATENCY_MODE}: ` +
      `supported=${r.supported}` +
      /* r.config 是浏览器回填的实际配置 —— 用它确认 latencyMode 是否被采纳 */
      (r.config ? ` codec=${r.config.codec} latencyMode=${r.config.latencyMode}` : ''),
  )
  if (!r.supported) {
    /* 不再静默降级 —— 实验需要确知跑的就是 High。
     * 若这里抛出，说明浏览器/硬件不支持该档位，需要改 level 或换 profile。 */
    throw new Error(
      `WebCodecs 不支持 H.264 High profile (${VIDEO_CODEC}) @ ${width}x${height}；` +
        `请查看控制台 probe 日志确认`,
    )
  }
  const chosen = (r.config ?? config) as VideoEncoderConfig
  log_info('encoder', `selected ${chosen.codec}`)
  return chosen
}

async function pickAudioCodec(): Promise<AudioEncoderConfig | null> {
  for (const codec of AUDIO_CODECS) {
    const config: AudioEncoderConfig = {
      codec,
      sampleRate: SAMPLE_RATE,
      numberOfChannels: CHANNELS,
      bitrate: AUDIO_BITRATE,
    }
    try {
      const r = await AudioEncoder.isConfigSupported(config)
      if (r.supported) return (r.config ?? config) as AudioEncoderConfig
    } catch {
      /* ignore */
    }
  }
  return null
}

export class AvcAacEncoder {
  private muxer: AvSink
  private video: VideoEncoder | null = null
  private audio: AudioEncoder | null = null
  private frameIndex = 0
  private originMs = 0
  private nextAudioUs = 0
  private lastKeyUs = -1
  private audioTsWarned = false
  private running = false
  private avcC: Uint8Array | null = null
  private aacAsc: Uint8Array | null = null
  private vcfg: VideoEncoderConfig | null = null
  private lastRecoverMs = 0
  private videoOut = 0
  private dropP = 0
  private lastBeatMs = 0
  /* ── 重排序（B 帧）观测 ── */
  private tsSeen = 0          /* 出帧总数 */
  private tsPrevMs = -1       /* 上一帧 PTS */
  private tsFirstMs = -1
  private tsLastMs = 0
  private tsBack = 0          /* PTS 回退次数（>0 说明有 B 帧） */
  private tsBackMax = 0       /* 最大单次回退（负数，越小回退越多） */
  private tsBackLast = 0      /* 最近一次回退量（负数） */
  private tsBackReported = 0  /* 已上报的回退次数 */
  private tsDup = 0           /* PTS 重复次数 */
  private tsMaxGap = 0        /* 最大正向间隔，用于判断帧率是否稳定 */
  status: EncoderStatus = {
    videoCodec: '',
    audioCodec: '',
    audioOk: false,
    width: 0,
    height: 0,
  }

  constructor(muxer: AvSink) {
    this.muxer = muxer
  }

  async start(width: number, height: number): Promise<EncoderStatus> {
    const vcfg = await pickVideoCodec(width, height)
    const acfg = await pickAudioCodec()

    this.status.width = width
    this.status.height = height
    this.status.videoCodec = vcfg.codec
    this.status.audioOk = !!acfg
    this.status.audioCodec = acfg?.codec ?? ''

    this.vcfg = vcfg
    this.setupVideoEncoder()
    log_info('encoder', `video configure ${vcfg.codec} ${width}x${height}`)

    if (acfg) {
      this.audio = new AudioEncoder({
        output: (chunk, meta) => this.onAudio(chunk, meta),
        error: (e) => log_error('encoder', `audio: ${e.message}`),
      })
      this.audio.configure(acfg)
      log_info('encoder', `audio configure ${acfg.codec}`)
    } else {
      log_warn('encoder', 'skip audio, no AAC encoder')
    }

    this.running = true
    this.frameIndex = 0
    this.originMs = performance.now()
    this.nextAudioUs = 0
    this.lastKeyUs = -1
    this.audioTsWarned = false
    this.videoOut = 0
    this.dropP = 0
    this.lastBeatMs = 0
    this.tsSeen = 0
    this.tsPrevMs = -1
    this.tsFirstMs = -1
    this.tsLastMs = 0
    this.tsBack = 0
    this.tsBackMax = 0
    this.tsBackLast = 0
    this.tsBackReported = 0
    this.tsDup = 0
    this.tsMaxGap = 0
    return this.status
  }

  private nowUs(): number {
    return Math.round((performance.now() - this.originMs) * 1000)
  }

  private setupVideoEncoder() {
    if (!this.vcfg) return
    try { this.video?.close() } catch { /* ignore */ }
    this.video = new VideoEncoder({
      output: (chunk, meta) => this.onVideo(chunk, meta),
      error: (e) => {
        log_error('encoder', `video: ${e.message}`)
        this.recoverVideoEncoder()
      },
    })
    this.video.configure(this.vcfg)
  }

  private recoverVideoEncoder() {
    if (!this.running || !this.vcfg) return
    const now = performance.now()
    if (now - this.lastRecoverMs < 1000) return
    this.lastRecoverMs = now
    this.lastKeyUs = -1
    log_warn('encoder', 'reconfigure VideoEncoder')
    this.setupVideoEncoder()
  }

  private onVideo(chunk: EncodedVideoChunk, meta?: EncodedVideoChunkMetadata) {
    const desc = meta?.decoderConfig?.description
    /* chunk.timestamp 是送入 VideoFrame 的呈现时间（PTS），不是 DTS。
     * 下面用「输出顺序 + 时间戳单调性」反推是否存在重排序（B 帧）。 */
    const ptsMs = Math.max(0, Math.round(chunk.timestamp / 1000))
    this.checkReorder(ptsMs, chunk.type)

    if (desc && !this.avcC) {
      this.avcC = asU8(desc)
      log_info(
        'encoder',
        `AVC seq header ${this.avcC.byteLength}B pts=${ptsMs} hex=${hex_preview(this.avcC)}`,
      )
      this.muxer.writeAvcSeq(this.avcC, ptsMs)
    }
    if (!this.avcC) return
    /* cts 传 0 —— 下游 FLV tag 里 pts 会被写成等于 dts。
     * 若 checkReorder 报出重排序，这里传 0 就是错的，需要传真实 CTS。 */
    this.muxer.writeAvcNalu(chunkBytes(chunk), ptsMs, 0, chunk.type === 'key')
  }

  /* ── 重排序（B 帧）检测 ──
   * WebCodecs 不暴露 DTS，EncodedVideoChunk.timestamp 是 PTS，
   * 输出顺序则是解码顺序。于是：
   *   - 编码器按 PTS 递增顺序输出  → 无重排序（无 B 帧），dts == pts
   *   - 出现 PTS 回退（后一帧 PTS 小于前一帧，超出容忍抖动）
   *                              → 有重排序（B 帧），dts != pts
   * 同时统计时间戳间隔，便于判断是否恒定帧率。
   * 仅做观测，不改变行为。 */
  private checkReorder(ptsMs: number, type: EncodedVideoChunkType) {
    this.tsSeen++
    this.tsFirstMs = this.tsFirstMs < 0 ? ptsMs : this.tsFirstMs
    this.tsLastMs = ptsMs

    if (this.tsPrevMs >= 0) {
      const delta = ptsMs - this.tsPrevMs
      if (delta === 0) this.tsDup++
      if (delta < 0) {
        /* 容忍 1ms 的取整抖动，避免误报 */
        if (delta < -1) {
          this.tsBack++
          this.tsBackLast = delta
          this.tsBackMax = Math.min(this.tsBackMax, delta)
        }
      } else if (this.tsMaxGap < delta) {
        this.tsMaxGap = delta
      }
    }
    this.tsPrevMs = ptsMs

    /* 出现新的回退就立刻告警一次；否则每 150 帧打一次统计 */
    if (this.tsBack > this.tsBackReported) {
      this.tsBackReported = this.tsBack
      log_warn(
        'encoder',
        `视频重排序检测: 第 ${this.tsSeen} 帧 PTS=${ptsMs} 相对上一帧回退 ` +
          `${-this.tsBackLast}ms；累计回退 ${this.tsBack} 次` +
          `（最大单次 ${-this.tsBackMax}ms）。说明存在 B 帧 → dts != pts，` +
          `而当前 muxer 把 cts 写死 0`,
      )
    } else if (this.tsSeen % 150 === 0) {
      const fps =
        this.tsLastMs > this.tsFirstMs
          ? ((this.tsSeen - 1) * 1000) / (this.tsLastMs - this.tsFirstMs)
          : 0
      log_info(
        'encoder',
        `视频时间戳统计: ${this.tsSeen} 帧 PTS ${this.tsFirstMs}..${this.tsLastMs}ms ` +
          `≈${fps.toFixed(1)}fps 回退=${this.tsBack} 重复=${this.tsDup} 最大间隔=${this.tsMaxGap}ms ` +
          `→ ${this.tsBack > 0 ? '存在重排序(dts!=pts)' : '无重排序(dts==pts)'}`,
      )
    }
  }

  private audioDts(chunk: EncodedAudioChunk): number {
    let tsUs = Number(chunk.timestamp)
    const durUs = Number(chunk.duration) > 0
      ? Number(chunk.duration)
      : Math.round(1024 / SAMPLE_RATE * 1e6)
    const wallUs = this.nowUs()
    /* Chrome 有时给出“正的但落后墙钟很远”的 timestamp，不能只判断 <=0 */
    if (!Number.isFinite(tsUs) || tsUs <= 0 || tsUs + 1_000_000 < wallUs) {
      if (!this.audioTsWarned) {
        this.audioTsWarned = true
        log_warn(
          'encoder',
          `AudioEncoder timestamp=${String(chunk.timestamp)} wall=${wallUs}，改用本地时钟`,
        )
      }
      tsUs = Math.max(wallUs, this.nextAudioUs)
    }
    if (tsUs < this.nextAudioUs) tsUs = this.nextAudioUs
    this.nextAudioUs = tsUs + durUs
    return Math.round(tsUs / 1000)
  }

  private onAudio(chunk: EncodedAudioChunk, meta?: EncodedAudioChunkMetadata) {
    const desc = meta?.decoderConfig?.description
    if (desc && !this.aacAsc) {
      this.aacAsc = asU8(desc)
      log_info('encoder', `AAC ASC ${this.aacAsc.byteLength}B hex=${hex_preview(this.aacAsc)}`)
    }
    const dts = this.audioDts(chunk)
    if (!this.aacAsc) this.aacAsc = aacLc48kStereoAsc()
    this.muxer.writeAacSeq(this.aacAsc, dts)
    this.muxer.writeAacRaw(chunkBytes(chunk), dts)
  }

  encodeVideo(frame: VideoFrame) {
    try {
      if (!this.running || !this.video) return
      if (this.video.state !== 'configured') {
        this.recoverVideoEncoder()
        return
      }
      const tsUs = this.nowUs()
      const keyFrame = this.lastKeyUs < 0 || tsUs - this.lastKeyUs >= KEYFRAME_INTERVAL_US
      this.frameIndex++
      const q = this.video.encodeQueueSize
      /* 只丢 P 帧。丢掉后不要强迫下一帧变关键帧，否则队列会堆满 IDR 再堵死 */
      if (!keyFrame && q > 2) {
        this.dropP++
        return
      }
      if (q > 6) {
        log_warn('encoder', `video queue=${q}，重建编码器`)
        this.recoverVideoEncoder()
        return
      }
      const stamped = new VideoFrame(frame, { timestamp: tsUs })
      try {
        this.video.encode(stamped, { keyFrame })
        if (keyFrame) this.lastKeyUs = tsUs
        this.videoOut++
        const now = performance.now()
        if (now - this.lastBeatMs > 2000) {
          this.lastBeatMs = now
          log_debug(
            'encoder',
            `video beat out=${this.videoOut} dropP=${this.dropP} q=${q} key=${keyFrame ? 1 : 0}`,
          )
        }
      } finally {
        stamped.close()
      }
    } catch (e) {
      log_error('encoder', `encodeVideo: ${e instanceof Error ? e.message : String(e)}`)
      this.recoverVideoEncoder()
    } finally {
      frame.close()
    }
  }

  encodePcmStereo48k(left: Float32Array, right: Float32Array) {
    if (!this.running || !this.audio) return
    const frames = left.length
    const planar = new Float32Array(frames * 2)
    planar.set(left, 0)
    planar.set(right, frames)
    const data = new AudioData({
      format: 'f32-planar',
      sampleRate: SAMPLE_RATE,
      numberOfFrames: frames,
      numberOfChannels: CHANNELS,
      timestamp: this.nowUs(),
      data: planar,
    })
    this.audio.encode(data)
    data.close()
  }

  async stop() {
    this.running = false
    try { await this.video?.flush() } catch { /* ignore */ }
    try { await this.audio?.flush() } catch { /* ignore */ }
    this.video?.close()
    this.audio?.close()
    this.video = null
    this.audio = null
  }
}

export { SAMPLE_RATE, CHANNELS, VIDEO_FPS }
