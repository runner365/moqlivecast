import { hex_preview, log_debug, log_error, log_info, log_warn } from '../logger'
import type { AvSink } from './avSink'
import { aacLc48kStereoAsc } from './flvMuxer'

const SAMPLE_RATE = 48000
const CHANNELS = 2
const VIDEO_FPS = 15
const VIDEO_BITRATE = 800_000
const AUDIO_BITRATE = 128_000
const KEYFRAME_INTERVAL_US = 4_000_000

const VIDEO_CODECS = ['avc1.64001f', 'avc1.4d001f', 'avc1.42E01E']
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
  for (const codec of VIDEO_CODECS) {
    const config: VideoEncoderConfig = {
      codec,
      width,
      height,
      bitrate: VIDEO_BITRATE,
      framerate: VIDEO_FPS,
      latencyMode: 'realtime',
      avc: { format: 'avc' },
    }
    const r = await VideoEncoder.isConfigSupported(config)
    if (r.supported) return (r.config ?? config) as VideoEncoderConfig
  }
  throw new Error('This browser does not support WebCodecs H.264 encoding')
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
    const dts = Math.max(0, Math.round(chunk.timestamp / 1000))
    if (desc && !this.avcC) {
      this.avcC = asU8(desc)
      log_info('encoder', `AVC seq header ${this.avcC.byteLength}B dts=${dts} hex=${hex_preview(this.avcC)}`)
      this.muxer.writeAvcSeq(this.avcC, dts)
    }
    if (!this.avcC) return
    this.muxer.writeAvcNalu(chunkBytes(chunk), dts, 0, chunk.type === 'key')
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
