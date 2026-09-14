import { log_error, log_info, log_warn } from '../logger'
import { FlvMuxer } from './flvMuxer'
import { AvcAacEncoder, SAMPLE_RATE, type EncoderStatus } from './encoder'
import { FlvDemuxer } from './flvdemuxer'
import { MediaStats, type MediaStatsSnapshot } from './mediaStats'
import { MoqPublisher } from './moqPublisher'
import type { SessionPhase, SessionPhaseHooks } from './sessionPhase'
import { WtPusher } from './wtPusher'

export type PushFormat = 'flv' | 'moq'

export type PushCallbacks = {
  onStatus?: (s: EncoderStatus) => void
  onError?: (msg: string) => void
  onStats?: (s: MediaStatsSnapshot) => void
  onTx?: (msg: string) => void
} & SessionPhaseHooks

type TrackProcessorCtor = new (init: { track: MediaStreamTrack }) => {
  readable: ReadableStream<VideoFrame>
}

export class PushPipeline {
  private stream: MediaStream | null = null
  private encoder: AvcAacEncoder | null = null
  private pusher = new WtPusher()
  private publisher: MoqPublisher | null = null
  private format: PushFormat = 'flv'
  private stats = new MediaStats()
  private demux = new FlvDemuxer('flv-push')
  private audioCtx: AudioContext | null = null
  private processor: ScriptProcessorNode | null = null
  private rvfcHandle = 0
  private videoTimer = 0
  private lastGrabMs = 0
  private trackReader: ReadableStreamDefaultReader<VideoFrame> | null = null
  private videoEl: HTMLVideoElement | null = null
  private running = false
  private onPhase: ((p: SessionPhase) => void) | null = null
  private mediaSeen = false

  get mediaStream() {
    return this.stream
  }

  async start(
    url: string,
    videoEl: HTMLVideoElement,
    cb: PushCallbacks = {},
    format: PushFormat = 'flv',
  ) {
    await this.stop()
    this.videoEl = videoEl
    this.format = format
    this.onPhase = cb.onPhase ?? null
    this.mediaSeen = false

    log_info('push', `start format=${format} ${url}`)
    this.stream = await navigator.mediaDevices.getUserMedia({
      video: {
        aspectRatio: 16 / 9,
        width: { ideal: 640 },
        height: { ideal: 360 },
        frameRate: { ideal: 15 },
      },
      audio: {
        sampleRate: SAMPLE_RATE,
        channelCount: 2,
        echoCancellation: true,
        noiseSuppression: true,
      },
    })

    videoEl.srcObject = this.stream
    videoEl.muted = true
    await videoEl.play()

    const vtrack = this.stream.getVideoTracks()[0]
    const settings = vtrack.getSettings()
    const width = settings.width || videoEl.videoWidth || 640
    const height = settings.height || videoEl.videoHeight || 360

    log_info('push', `camera ${width}x${height} tracks=${this.stream.getTracks().length}`)

    if (format === 'moq') {
      const publisher = new MoqPublisher()
      this.publisher = publisher
      this.encoder = new AvcAacEncoder(publisher)
      const status = await this.encoder.start(width, height)
      log_info(
        'push',
        `encoder video=${status.videoCodec} audio=${status.audioOk ? status.audioCodec : 'none'}`,
      )
      cb.onStatus?.(status)
      if (!status.audioOk) {
        log_warn('push', 'WebCodecs has no AAC, video-only H.264')
        cb.onError?.('This browser cannot encode AAC with WebCodecs; publishing H.264 only')
      }
      await publisher.connect(url, status, {
        onStats: (s) => cb.onStats?.(s),
        onTx: (msg) => cb.onTx?.(msg),
        onClosed: () => this.setPhase('disconnected'),
      })
    } else {
      await this.pusher.connect(url, () => this.setPhase('disconnected'))
      this.stats.start((s) => cb.onStats?.(s))
      this.demux = new FlvDemuxer('flv-push', (kind, size, ts, key) => {
        if (kind === 'video') this.stats.addVideo(size, ts, key)
        else this.stats.addAudio(size)
      })
      const muxer = new FlvMuxer((buf) => {
        this.demux.push(buf)
        this.pusher.push(buf)
      })
      this.encoder = new AvcAacEncoder(muxer)
      const status = await this.encoder.start(width, height)
      log_info(
        'push',
        `encoder video=${status.videoCodec} audio=${status.audioOk ? status.audioCodec : 'none'}`,
      )
      cb.onStatus?.(status)
      if (!status.audioOk) {
        log_warn('push', 'WebCodecs has no AAC, video-only H.264')
        cb.onError?.('This browser cannot encode AAC with WebCodecs; publishing H.264 only')
      }
    }

    this.setPhase('connected and waiting for stream')
    this.running = true
    this.startVideoLoop(videoEl)
    this.startAudio(this.stream)
  }

  private setPhase(phase: SessionPhase) {
    this.onPhase?.(phase)
  }

  private markPushing() {
    if (this.mediaSeen || !this.running) return
    this.mediaSeen = true
    this.setPhase('pushing stream')
  }

  private startVideoLoop(videoEl: HTMLVideoElement) {
    const track = this.stream?.getVideoTracks()[0]
    const Processor = (globalThis as unknown as { MediaStreamTrackProcessor?: TrackProcessorCtor })
      .MediaStreamTrackProcessor
    if (track && Processor) {
      log_info('push', 'video capture=MediaStreamTrackProcessor')
      void this.readVideoTrack(track, Processor)
      return
    }
    log_warn('push', '无 MediaStreamTrackProcessor，回退 rVFC + 定时抓帧')
    this.startRvfc(videoEl)
    this.videoTimer = window.setInterval(() => {
      if (!this.running) return
      if (performance.now() - this.lastGrabMs < 80) return
      this.grabFromElement(videoEl)
    }, 33)
  }

  private async readVideoTrack(track: MediaStreamTrack, Processor: TrackProcessorCtor) {
    const proc = new Processor({ track })
    this.trackReader = proc.readable.getReader()
    try {
      while (this.running && this.trackReader) {
        const { value, done } = await this.trackReader.read()
        if (done) break
        if (value) {
          this.lastGrabMs = performance.now()
          this.markPushing()
          this.encoder?.encodeVideo(value)
        }
      }
    } catch (e) {
      if (this.running) {
        log_error('push', `video track: ${e instanceof Error ? e.message : String(e)}`)
      }
    }
  }

  private startRvfc(videoEl: HTMLVideoElement) {
    const tick = () => {
      if (!this.running || !this.encoder) return
      this.grabFromElement(videoEl)
      this.rvfcHandle = videoEl.requestVideoFrameCallback(tick)
    }
    this.rvfcHandle = videoEl.requestVideoFrameCallback(tick)
  }

  private grabFromElement(videoEl: HTMLVideoElement) {
    try {
      if (videoEl.readyState < 2) return
      this.lastGrabMs = performance.now()
      this.markPushing()
      this.encoder?.encodeVideo(new VideoFrame(videoEl, { timestamp: 0 }))
    } catch (e) {
      log_error('push', `grab frame: ${e instanceof Error ? e.message : String(e)}`)
    }
  }

  private startAudio(stream: MediaStream) {
    if (!this.encoder?.status.audioOk) return
    const tracks = stream.getAudioTracks()
    if (!tracks.length) return

    this.audioCtx = new AudioContext({ sampleRate: SAMPLE_RATE })
    const src = this.audioCtx.createMediaStreamSource(stream)
    this.processor = this.audioCtx.createScriptProcessor(1024, 2, 2)
    this.processor.onaudioprocess = (ev) => {
      if (!this.running || !this.encoder) return
      const inBuf = ev.inputBuffer
      const left = inBuf.getChannelData(0)
      const right = inBuf.numberOfChannels > 1
        ? inBuf.getChannelData(1)
        : left
      this.encoder.encodePcmStereo48k(
        new Float32Array(left),
        new Float32Array(right),
      )
    }
    const silent = this.audioCtx.createGain()
    silent.gain.value = 0
    src.connect(this.processor)
    this.processor.connect(silent)
    silent.connect(this.audioCtx.destination)
    void this.audioCtx.resume()
  }

  async stop() {
    if (this.running) log_info('push', 'stop')
    this.running = false
    this.onPhase = null
    if (this.videoTimer) {
      window.clearInterval(this.videoTimer)
      this.videoTimer = 0
    }
    try { await this.trackReader?.cancel() } catch { /* ignore */ }
    this.trackReader = null
    if (this.videoEl && this.rvfcHandle) {
      this.videoEl.cancelVideoFrameCallback(this.rvfcHandle)
      this.rvfcHandle = 0
    }
    this.processor?.disconnect()
    this.processor = null
    try { await this.audioCtx?.close() } catch { /* ignore */ }
    this.audioCtx = null
    await this.encoder?.stop()
    this.encoder = null
    this.stats.stop()
    this.demux.reset()
    await this.publisher?.close()
    this.publisher = null
    await this.pusher.close()
    this.stream?.getTracks().forEach((t) => t.stop())
    this.stream = null
    if (this.videoEl) {
      this.videoEl.srcObject = null
      this.videoEl = null
    }
  }
}
