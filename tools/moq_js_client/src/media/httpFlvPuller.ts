import flvjs from 'flv.js'
import { log_error, log_info, log_warn } from '../logger'
import { EMPTY_STATS, type MediaStatsSnapshot } from './mediaStats'
import type { SessionPhase, SessionPhaseHooks } from './sessionPhase'
import { MEDIA_STALL_MS, StallWatch } from './stallWatch'

const LIVE_LATENCY_S = 1.0
const MAX_LATENCY_S = 2.5

type BufRange = { start: number; end: number }

export class HttpFlvPuller {
  private player: ReturnType<typeof flvjs.createPlayer> | null = null
  private videoEl: HTMLVideoElement | null = null
  private closed = true
  private chaseTimer = 0
  private lastSeekMs = 0
  private lastDecoded = 0
  private lastStatAt = 0
  private onStats: ((s: MediaStatsSnapshot) => void) | null = null
  private onPhase: ((p: SessionPhase) => void) | null = null
  private mediaSeen = false
  private stall = new StallWatch()

  private readonly onVideoError = () => {
    const err = this.videoEl?.error
    log_error('http-flv', `video.error code=${err?.code} msg=${err?.message ?? ''}`)
  }
  private readonly onVideoPlaying = () => {
    log_info('http-flv', `video playing ${this.bufferLog()}`)
  }
  private readonly onVideoWaiting = () => {
    log_warn('http-flv', `video waiting ${this.bufferLog()}`)
    this.chaseLive('waiting')
    void this.videoEl?.play()?.catch(() => {})
  }
  private readonly onVideoStalled = () => {
    log_warn('http-flv', `video stalled ${this.bufferLog()}`)
    this.chaseLive('stalled')
  }
  private readonly onLoadedMeta = () => {
    const v = this.videoEl
    log_info(
      'http-flv',
      `video loadedmetadata ${v?.videoWidth}x${v?.videoHeight} ${this.bufferLog()}`,
    )
    this.chaseLive('metadata')
  }

  async start(
    url: string,
    video: HTMLVideoElement,
    hooks: { onStats?: (s: MediaStatsSnapshot) => void } & SessionPhaseHooks = {},
  ) {
    await this.stop()
    this.closed = false
    this.mediaSeen = false
    this.videoEl = video
    this.onStats = hooks.onStats ?? null
    this.onPhase = hooks.onPhase ?? null
    this.lastDecoded = 0
    this.lastStatAt = 0
    this.lastSeekMs = 0
    hooks.onStats?.({ ...EMPTY_STATS })

    if (!flvjs.isSupported()) {
      throw new Error('This browser does not support flv.js / MSE')
    }

    log_info('http-flv', `connect ${url}`)
    this.player = flvjs.createPlayer(
      { type: 'flv', isLive: true, url },
      {
        enableStashBuffer: false,
        stashInitialSize: 128,
        lazyLoad: false,
        deferLoadAfterSourceOpen: false,
        fixAudioTimestampGap: false,
        autoCleanupSourceBuffer: true,
        autoCleanupMaxBackwardDuration: 8,
        autoCleanupMinBackwardDuration: 4,
      },
    )
    this.bindPlayerEvents()
    this.bindVideoEvents(video)
    video.muted = false
    video.volume = 1
    this.player.attachMediaElement(video)
    this.player.load()
    log_info('http-flv', 'flv.js load()')

    void this.player.play()?.then(() => {
      video.muted = false
      video.volume = 1
    }).catch((e: unknown) => {
      log_warn('http-flv', `play rejected: ${e instanceof Error ? e.message : String(e)}`)
    })

    this.chaseTimer = window.setInterval(() => this.chaseLive('timer'), 500)
    this.setPhase('connected and waiting for stream')
  }

  async stop() {
    this.closed = true
    this.stall.stop()
    if (this.chaseTimer) {
      window.clearInterval(this.chaseTimer)
      this.chaseTimer = 0
    }
    this.unbindVideoEvents()
    this.videoEl = null
    this.onStats = null
    try { this.player?.pause() } catch { /* ignore */ }
    try { this.player?.unload() } catch { /* ignore */ }
    try { this.player?.detachMediaElement() } catch { /* ignore */ }
    try { this.player?.destroy() } catch { /* ignore */ }
    this.player = null
    this.onPhase = null
    log_info('http-flv', 'stopped')
  }

  private setPhase(phase: SessionPhase) {
    if (this.closed && phase !== 'disconnected') return
    this.onPhase?.(phase)
  }

  private noteMedia() {
    if (this.closed) return
    if (!this.mediaSeen) {
      this.mediaSeen = true
      this.stall.start(() => this.onMediaStall())
      this.setPhase('receiving stream')
      return
    }
    this.stall.touch()
  }

  private onMediaStall() {
    if (this.closed) return
    log_warn('http-flv', `no media for ${MEDIA_STALL_MS}ms, disconnect`)
    this.setPhase('disconnected')
    void this.stop()
  }

  private bindPlayerEvents() {
    if (!this.player) return
    this.player.on(flvjs.Events.ERROR, (type: unknown, detail: unknown, info: unknown) => {
      log_error('flv.js', `ERROR type=${String(type)} detail=${String(detail)}`, info)
      this.setPhase('disconnected')
    })
    this.player.on(flvjs.Events.MEDIA_INFO, (info: unknown) => {
      log_info('flv.js', 'MEDIA_INFO', info)
      this.noteMedia()
    })
    this.player.on(flvjs.Events.STATISTICS_INFO, (info: unknown) => {
      this.onStatistics(info)
    })
  }

  private onStatistics(info: unknown) {
    if (this.closed || !this.onStats) return
    const s = info as { speed?: number; decodedFrames?: number }
    if (typeof s.speed === 'number' && s.speed > 0) this.noteMedia()
    const now = performance.now()
    let fps: number | null = null
    if (typeof s.decodedFrames === 'number' && this.lastStatAt > 0) {
      const dt = (now - this.lastStatAt) / 1000
      if (dt > 0.2) fps = (s.decodedFrames - this.lastDecoded) / dt
    }
    if (typeof s.decodedFrames === 'number') {
      this.lastDecoded = s.decodedFrames
      this.lastStatAt = now
    }
    this.onStats({
      videoFps: fps != null && Number.isFinite(fps) ? fps : null,
      videoKbps: typeof s.speed === 'number' ? s.speed * 8 : null,
      audioKbps: null,
      gopSec: null,
    })
  }

  private bufferRanges(): BufRange[] {
    const v = this.videoEl
    if (!v) return []
    const out: BufRange[] = []
    for (let i = 0; i < v.buffered.length; i++) {
      out.push({ start: v.buffered.start(i), end: v.buffered.end(i) })
    }
    return out
  }

  private bufferLog(): string {
    const v = this.videoEl
    if (!v) return ''
    const ranges = this.bufferRanges()
    const text = ranges.map((r) => `[${r.start.toFixed(2)},${r.end.toFixed(2)}]`).join(' ')
    const end = ranges.length ? ranges[ranges.length - 1].end : 0
    const lag = ranges.length ? end - v.currentTime : 0
    return `t=${v.currentTime.toFixed(2)} lag=${lag.toFixed(2)} ready=${v.readyState} paused=${v.paused} buf=${text || '-'}`
  }

  private rangeContaining(t: number, ranges: BufRange[]): BufRange | null {
    for (const r of ranges) {
      if (t >= r.start - 0.02 && t < r.end) return r
    }
    return null
  }

  private seekInBuffer(target: number, reason: string) {
    const v = this.videoEl
    const p = this.player
    const ranges = this.bufferRanges()
    if (!v || !p || ranges.length === 0) return

    let hit = this.rangeContaining(target, ranges)
    if (!hit) {
      hit = ranges.find((r) => r.start > target) ?? ranges[ranges.length - 1]
      target = hit.start + 0.04
    }
    const maxT = Math.max(hit.start, hit.end - 0.08)
    if (target > maxT) target = maxT
    if (Math.abs(v.currentTime - target) < 0.05) return

    const now = performance.now()
    if (now - this.lastSeekMs < 400) return
    this.lastSeekMs = now
    log_info('http-flv', `seek ${reason} -> ${target.toFixed(2)} ${this.bufferLog()}`)
    p.currentTime = target
    void v.play()?.catch(() => {})
  }

  private chaseLive(reason: string) {
    const v = this.videoEl
    if (!v) return
    const ranges = this.bufferRanges()
    if (ranges.length === 0) return

    const last = ranges[ranges.length - 1]
    const t = v.currentTime
    const here = this.rangeContaining(t, ranges)
    if (!here) {
      const next = ranges.find((r) => r.start > t)
      if (next) this.seekInBuffer(next.start + 0.04, `${reason}:hole`)
      else if (t < last.start) this.seekInBuffer(last.start + 0.04, `${reason}:behind`)
      else if (t >= last.end) this.seekInBuffer(Math.max(last.start, last.end - 0.05), `${reason}:ahead`)
      return
    }
    const lag = last.end - t
    if (lag > MAX_LATENCY_S) {
      this.seekInBuffer(last.end - LIVE_LATENCY_S, `${reason}:lag=${lag.toFixed(2)}`)
    }
  }

  private bindVideoEvents(video: HTMLVideoElement) {
    video.addEventListener('error', this.onVideoError)
    video.addEventListener('playing', this.onVideoPlaying)
    video.addEventListener('waiting', this.onVideoWaiting)
    video.addEventListener('stalled', this.onVideoStalled)
    video.addEventListener('loadedmetadata', this.onLoadedMeta)
  }

  private unbindVideoEvents() {
    const v = this.videoEl
    if (!v) return
    v.removeEventListener('error', this.onVideoError)
    v.removeEventListener('playing', this.onVideoPlaying)
    v.removeEventListener('waiting', this.onVideoWaiting)
    v.removeEventListener('stalled', this.onVideoStalled)
    v.removeEventListener('loadedmetadata', this.onLoadedMeta)
  }
}
