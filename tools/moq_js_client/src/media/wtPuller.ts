import flvjs from 'flv.js'
import { hex_preview, log_debug, log_error, log_info, log_warn } from '../logger'
import { FlvChunkLoader, FlvChunkSource, bindFlvChunkSource } from './flvChunkLoader'
import { FlvDemuxer } from './flvdemuxer'
import { MediaStats, type MediaStatsSnapshot } from './mediaStats'
import type { SessionPhase, SessionPhaseHooks } from './sessionPhase'
import { MEDIA_STALL_MS, StallWatch } from './stallWatch'

const OPEN_STREAM_MSG = 'open stream'
const LIVE_LATENCY_S = 1.0
const MAX_LATENCY_S = 2.5

type BufRange = { start: number; end: number }

export class WtPuller {
  private wt: WebTransport | null = null
  private reader: ReadableStreamDefaultReader<Uint8Array> | null = null
  private writer: WritableStreamDefaultWriter<Uint8Array> | null = null
  private player: ReturnType<typeof flvjs.createPlayer> | null = null
  private source = new FlvChunkSource()
  private demux = new FlvDemuxer()
  private stats = new MediaStats()
  private closed = false
  private reading = false
  private recvChunks = 0
  private recvBytes = 0
  private videoEl: HTMLVideoElement | null = null
  private chaseTimer = 0
  private lastSeekMs = 0
  private onPhase: ((p: SessionPhase) => void) | null = null
  private mediaSeen = false
  private stall = new StallWatch()
  private connGen = 0

  private readonly onVideoError = () => {
    const err = this.videoEl?.error
    log_error('wt-pull', `video.error code=${err?.code} msg=${err?.message ?? ''}`)
  }
  private readonly onVideoPlaying = () => {
    log_info('wt-pull', `video playing ${this.bufferLog()}`)
  }
  private readonly onVideoWaiting = () => {
    log_warn('wt-pull', `video waiting ${this.bufferLog()}`)
    this.resumeTransmuxer()
    this.chaseLive('waiting')
    void this.videoEl?.play()?.catch(() => {})
  }
  private readonly onVideoStalled = () => {
    log_warn('wt-pull', `video stalled ${this.bufferLog()}`)
    this.resumeTransmuxer()
    this.chaseLive('stalled')
  }
  private readonly onLoadedMeta = () => {
    const v = this.videoEl
    log_info('wt-pull', `video loadedmetadata ${v?.videoWidth}x${v?.videoHeight} ${this.bufferLog()}`)
    this.chaseLive('metadata')
  }

  async start(
    url: string,
    video: HTMLVideoElement,
    hooks: { onStats?: (s: MediaStatsSnapshot) => void } & SessionPhaseHooks = {},
  ) {
    await this.stop()
    const gen = ++this.connGen
    this.closed = false
    this.mediaSeen = false
    this.onPhase = hooks.onPhase ?? null
    this.source = new FlvChunkSource()
    this.stats.start((s) => hooks.onStats?.(s))
    this.demux = new FlvDemuxer('flv-demux', (kind, size, ts, key) => {
      if (kind === 'video') this.stats.addVideo(size, ts, key)
      else this.stats.addAudio(size)
    })
    this.recvChunks = 0
    this.recvBytes = 0
    this.lastSeekMs = 0
    this.videoEl = video

    log_info('wt-pull', `connect ${url}`)
    const wt = new WebTransport(url)
    this.wt = wt
    await wt.ready
    if (gen !== this.connGen || this.wt !== wt) return
    log_info('wt-pull', 'WebTransport ready')
    this.setPhase('connected and waiting for stream')

    const bidi = await wt.createBidirectionalStream()
    if (gen !== this.connGen || this.wt !== wt) return
    this.writer = bidi.writable.getWriter()
    this.reader = bidi.readable.getReader()
    log_info('wt-pull', 'bidi stream opened')

    if (!flvjs.isSupported()) {
      throw new Error('This browser does not support flv.js / MSE')
    }

    try {
      flvjs.LoggingControl.applyConfig({
        enableDebug: false,
        enableVerbose: false,
        enableInfo: false,
        enableWarn: true,
        enableError: true,
      })
    } catch (e) {
      log_warn('wt-pull', `flv.js LoggingControl 不可用: ${String(e)}`)
    }

    bindFlvChunkSource(this.source)
    this.player = flvjs.createPlayer(
      {
        type: 'flv',
        isLive: true,
        url: 'wt://moq-pull',
      },
      {
        enableStashBuffer: false,
        stashInitialSize: 128,
        lazyLoad: false,
        deferLoadAfterSourceOpen: false,
        fixAudioTimestampGap: false,
        autoCleanupSourceBuffer: true,
        autoCleanupMaxBackwardDuration: 8,
        autoCleanupMinBackwardDuration: 4,
        customLoader: FlvChunkLoader as never,
      },
    )
    this.bindPlayerEvents()
    this.bindVideoEvents(video)
    video.muted = false
    video.volume = 1
    this.player.attachMediaElement(video)
    this.player.load()
    log_info('wt-pull', 'flv.js load()')

    const msg = new TextEncoder().encode(OPEN_STREAM_MSG)
    await this.writer.write(msg)
    log_info('wt-pull', `sent "${OPEN_STREAM_MSG}" ${msg.byteLength} bytes`)

    void this.player.play()?.then(() => {
      video.muted = false
      video.volume = 1
    }).catch((e: unknown) => {
      log_warn('wt-pull', `flv.js play() rejected: ${e instanceof Error ? e.message : String(e)}`)
    })

    wt.closed.catch((e) => {
      if (gen !== this.connGen || this.wt !== wt) return
      this.closed = true
      this.setPhase('disconnected')
      log_warn('wt-pull', `WebTransport closed: ${e instanceof Error ? e.message : String(e)}`)
    })

    this.chaseTimer = window.setInterval(() => this.chaseLive('timer'), 500)
    void this.readLoop()
  }

  private bindPlayerEvents() {
    if (!this.player) return
    this.player.on(flvjs.Events.ERROR, (type: unknown, detail: unknown, info: unknown) => {
      log_error('flv.js', `ERROR type=${String(type)} detail=${String(detail)}`, info)
    })
    this.player.on(flvjs.Events.MEDIA_INFO, (info: unknown) => {
      log_info('flv.js', 'MEDIA_INFO', info)
    })
    this.player.on(flvjs.Events.METADATA_ARRIVED, (meta: unknown) => {
      log_info('flv.js', 'METADATA_ARRIVED', meta)
    })
    this.player.on(flvjs.Events.STATISTICS_INFO, (info: unknown) => {
      log_debug('flv.js', 'STATISTICS_INFO', info)
    })
    this.player.on(flvjs.Events.LOADING_COMPLETE, () => {
      log_warn('flv.js', 'LOADING_COMPLETE (直播不应结束)')
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

  /* 只能 seek 到已缓冲区间。直接改 video.currentTime 会走 flv.js 未缓冲 seek，清掉整个 MSE。 */
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
    log_info('wt-pull', `seek ${reason} -> ${target.toFixed(2)} ${this.bufferLog()}`)
    p.currentTime = target
    void v.play()?.catch(() => {})
  }

  private resumeTransmuxer() {
    const p = this.player as unknown as {
      _transmuxer?: { resume?: () => void }
    } | null
    try {
      p?._transmuxer?.resume?.()
    } catch {
      /* ignore */
    }
  }

  /* 迟到拉流时 MSE 可能已有几十秒；currentTime 停在 0 或卡在空洞里都要跳 */
  private chaseLive(reason: string) {
    const v = this.videoEl
    if (!v) return
    const ranges = this.bufferRanges()
    if (ranges.length === 0) return

    const last = ranges[ranges.length - 1]
    const t = v.currentTime
    const here = this.rangeContaining(t, ranges)
    if (!here) {
      this.resumeTransmuxer()
      const next = ranges.find((r) => r.start > t)
      if (next) {
        this.seekInBuffer(next.start + 0.04, `${reason}:hole`)
      } else if (t < last.start) {
        this.seekInBuffer(last.start + 0.04, `${reason}:behind`)
      }
      /* 越过 buffer 末端：等新数据，避免 seek:ahead 死循环 */
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

  private async readLoop() {
    if (!this.reader || this.reading) return
    this.reading = true
    log_info('wt-pull', 'readLoop start')
    try {
      while (!this.closed && this.reader) {
        const { value, done } = await this.reader.read()
        if (done) {
          log_warn('wt-pull', `readLoop done chunks=${this.recvChunks} bytes=${this.recvBytes}`)
          break
        }
        if (value && value.byteLength > 0) {
          try {
            this.onFlvBytes(value)
          } catch (e) {
            log_error(
              'wt-pull',
              `feed flv.js failed: ${e instanceof Error ? e.message : String(e)}`,
            )
          }
        }
      }
    } catch (e) {
      if (!this.closed) {
        log_error('wt-pull', `readLoop error: ${e instanceof Error ? e.message : String(e)}`)
      } else {
        log_debug('wt-pull', 'readLoop abort after close')
      }
    } finally {
      this.reading = false
      log_info('wt-pull', `readLoop exit chunks=${this.recvChunks} bytes=${this.recvBytes}`)
    }
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
    log_warn('wt-pull', `no media for ${MEDIA_STALL_MS}ms, disconnect`)
    this.setPhase('disconnected')
    void this.stop()
  }

  /* pull 收到 FLV 的入口 */
  private onFlvBytes(value: Uint8Array) {
    this.recvChunks++
    this.recvBytes += value.byteLength
    this.noteMedia()
    if (this.recvChunks === 1) {
      log_info(
        'wt-pull',
        `FLV first chunk len=${value.byteLength} hex=${hex_preview(value)}`,
      )
    } else {
      log_debug(
        'wt-pull',
        `FLV 入口 chunk#${this.recvChunks} len=${value.byteLength} total=${this.recvBytes}`,
      )
    }
    this.demux.push(value)
    this.source.push(value)
  }

  async stop() {
    const wasOpen = !this.closed || !!this.wt
    this.connGen += 1
    this.closed = true
    this.stall.stop()
    if (wasOpen && this.recvChunks > 0) {
      log_info('wt-pull', `stop after chunks=${this.recvChunks} bytes=${this.recvBytes}`)
    }
    if (this.chaseTimer) {
      window.clearInterval(this.chaseTimer)
      this.chaseTimer = 0
    }
    this.unbindVideoEvents()
    this.videoEl = null
    try { this.reader?.releaseLock() } catch { /* ignore */ }
    this.reader = null
    try { await this.writer?.close() } catch { /* ignore */ }
    this.writer = null
    try { this.player?.pause() } catch { /* ignore */ }
    try { this.player?.unload() } catch { /* ignore */ }
    try { this.player?.detachMediaElement() } catch { /* ignore */ }
    try { this.player?.destroy() } catch { /* ignore */ }
    this.player = null
    bindFlvChunkSource(null)
    try { this.wt?.close() } catch { /* ignore */ }
    this.wt = null
    this.stats.stop()
    this.demux.reset()
  }
}
