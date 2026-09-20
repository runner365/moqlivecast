import { sessionStream } from '../config'
import { hex_preview, log_error, log_info, log_warn } from '../logger'
import {
  ALIAS_AUDIO,
  ALIAS_CATALOG,
  ALIAS_VIDEO,
  encodeCatalogJson,
  encodeObject,
  encodePublish,
  encodeSetup,
  encodeSubgroupHeader,
} from '../moq/wire'
import type { AvSink } from './avSink'
import type { EncoderStatus } from './encoder'
import { MediaStats, type MediaStatsSnapshot } from './mediaStats'

export type MoqTxHooks = {
  onStats?: (s: MediaStatsSnapshot) => void
  onTx?: (msg: string) => void
  onClosed?: () => void
}

function parseAppStream(url: string): { app: string; stream: string } {
  /* URL 缺参数时的兜底用 sessionStream()，而不是写死 '123456'：
   * 保持一致才能让「没填 stream 的推流」与「同标签页的拉流」对上。 */
  try {
    const u = new URL(url)
    return {
      app: u.searchParams.get('app') || 'live',
      stream: u.searchParams.get('stream') || sessionStream(),
    }
  } catch {
    return { app: 'live', stream: sessionStream() }
  }
}

export class MoqPublisher implements AvSink {
  private wt: WebTransport | null = null
  private control: WritableStreamDefaultWriter<Uint8Array> | null = null
  private videoWriter: WritableStreamDefaultWriter<Uint8Array> | null = null
  private audioWriter: WritableStreamDefaultWriter<Uint8Array> | null = null
  private closed = false
  private app = 'live'
  private stream = sessionStream()
  private videoGroup = -1
  private videoObj = 0
  private audioObj = 0
  private avcC: Uint8Array | null = null
  private aacAsc: Uint8Array | null = null
  private avcCSent = false
  private aacSent = false
  private videoChain: Promise<void> = Promise.resolve()
  private audioChain: Promise<void> = Promise.resolve()
  private stats = new MediaStats()
  private onTx: ((msg: string) => void) | null = null
  private beatTimer = 0
  private sinkVideo = 0
  private sinkAudio = 0
  private lastVideoDtsMs = -1
  private dropStaleAudio = 0
  private connGen = 0
  /* 诊断：发送链积压追踪（定位 ts 错乱用，定位完可删） */
  private videoPending = 0
  private audioPending = 0
  private lastVideoLagMs = 0
  private lastAudioLagMs = 0
  private maxVideoLagMs = 0
  private maxAudioLagMs = 0
  private lagLoggedV = 0
  private lagLoggedA = 0

  async connect(url: string, status: EncoderStatus, hooks: MoqTxHooks = {}) {
    await this.close()
    const gen = ++this.connGen
    this.closed = false
    this.onTx = hooks.onTx ?? null
    const id = parseAppStream(url)
    this.app = id.app
    this.stream = id.stream
    this.tx(`connect ${url} app=${this.app} stream=${this.stream}`)

    const wt = new WebTransport(url)
    this.wt = wt
    await wt.ready
    if (gen !== this.connGen || this.wt !== wt) return
    this.tx('WebTransport ready')
    wt.closed.catch(() => {
      if (gen !== this.connGen || this.wt !== wt) return
      this.closed = true
      hooks.onClosed?.()
    })

    const bidi = await this.wt.createBidirectionalStream()
    this.control = bidi.writable.getWriter()
    this.tx('control bidi opened')

    await this.sendControl(encodeSetup(), 'SETUP')
    await this.sendPublish(0, 'catalog', ALIAS_CATALOG)
    await this.sendPublish(2, 'video', ALIAS_VIDEO)
    await this.sendPublish(4, 'audio', ALIAS_AUDIO)
    this.tx('signaling PUBLISH done, next=bidi catalog/audio')
    await this.sendCatalog(status)

    this.audioWriter = await this.openMediaBidi(`audio alias=${ALIAS_AUDIO} group=0`)
    await this.write(this.audioWriter, encodeSubgroupHeader(ALIAS_AUDIO, 0),
      `SUBGROUP audio alias=${ALIAS_AUDIO} group=0`)

    this.videoGroup = 0
    this.videoWriter = await this.openMediaBidi(`video alias=${ALIAS_VIDEO} group=0`)
    await this.write(this.videoWriter, encodeSubgroupHeader(ALIAS_VIDEO, 0),
      `SUBGROUP video alias=${ALIAS_VIDEO} group=0`)

    this.stats.start((s) => hooks.onStats?.(s))
    this.startBeat()
    this.tx('signaling done, wait encoder objects')
  }

  writeAvcSeq(avcC: Uint8Array, dtsMs: number) {
    this.avcC = avcC
    this.tx(`sink video_config avcC=${avcC.byteLength}B dts=${dtsMs} hex=${hex_preview(avcC)}`)
  }

  writeAvcNalu(payload: Uint8Array, dtsMs: number, _ctsMs: number, key: boolean) {
    this.sinkVideo += 1
    this.lastVideoDtsMs = dtsMs
    if (this.closed) {
      if (this.sinkVideo <= 3) this.tx(`DROP video #${this.sinkVideo} closed=1 ${payload.byteLength}B`)
      return
    }
    if (this.sinkVideo <= 3 || this.sinkVideo % 30 === 0) {
      this.tx(`sink video #${this.sinkVideo} dts=${dtsMs} key=${key ? 1 : 0} ${payload.byteLength}B`)
    }
    const enqueuedAt = performance.now()
    this.videoPending += 1
    this.videoChain = this.videoChain
      .then(() => {
        const lagMs = Math.round(performance.now() - enqueuedAt)
        this.lastVideoLagMs = lagMs
        if (lagMs > this.maxVideoLagMs) this.maxVideoLagMs = lagMs
        if (lagMs > 500 && (this.lagLoggedV < 5 || this.lagLoggedV % 20 === 0)) {
          this.tx(`video chain lag=${lagMs}ms pending=${this.videoPending} dts=${dtsMs}`, 'warn')
        }
        if (lagMs > 500) this.lagLoggedV += 1
        return this.sendVideo(payload, dtsMs, key)
      })
      .catch((e) => {
        this.closed = true
        this.tx(`ERROR video: ${e instanceof Error ? e.message : String(e)}`, 'error')
      })
      .finally(() => { this.videoPending -= 1 })
  }

  writeAacSeq(asc: Uint8Array, dtsMs: number) {
    if (this.aacAsc) return
    this.aacAsc = asc
    this.tx(`sink audio_config ASC=${asc.byteLength}B dts=${dtsMs} hex=${hex_preview(asc)}`)
  }

  writeAacRaw(payload: Uint8Array, dtsMs: number) {
    this.sinkAudio += 1
    if (this.closed) {
      if (this.sinkAudio <= 3) this.tx(`DROP audio #${this.sinkAudio} closed=1 ${payload.byteLength}B`)
      return
    }
    /* audioChain 积压时 dts 会落后视频数分钟，拉流 GOP/base 被旧音频拖歪 */
    if (this.lastVideoDtsMs >= 0 && dtsMs + 1500 < this.lastVideoDtsMs) {
      this.dropStaleAudio += 1
      if (this.dropStaleAudio <= 3 || this.dropStaleAudio % 50 === 0) {
        this.tx(
          `DROP stale audio #${this.dropStaleAudio} dts=${dtsMs} video=${this.lastVideoDtsMs}`,
          'warn',
        )
      }
      return
    }
    if (this.sinkAudio <= 3 || this.sinkAudio % 50 === 0) {
      this.tx(`sink audio #${this.sinkAudio} dts=${dtsMs} ${payload.byteLength}B`)
    }
    const enqueuedAt = performance.now()
    this.audioPending += 1
    this.audioChain = this.audioChain
      .then(() => {
        const lagMs = Math.round(performance.now() - enqueuedAt)
        this.lastAudioLagMs = lagMs
        if (lagMs > this.maxAudioLagMs) this.maxAudioLagMs = lagMs
        if (lagMs > 500 && (this.lagLoggedA < 5 || this.lagLoggedA % 20 === 0)) {
          this.tx(`audio chain lag=${lagMs}ms pending=${this.audioPending} dts=${dtsMs}`, 'warn')
        }
        if (lagMs > 500) this.lagLoggedA += 1
        return this.sendAudio(payload, dtsMs)
      })
      .catch((e) => {
        this.closed = true
        this.tx(`ERROR audio: ${e instanceof Error ? e.message : String(e)}`, 'error')
      })
      .finally(() => { this.audioPending -= 1 })
  }

  async close() {
    this.connGen += 1
    this.closed = true
    this.stopBeat()
    this.stats.stop()
    try { await this.videoWriter?.close() } catch { /* ignore */ }
    try { await this.audioWriter?.close() } catch { /* ignore */ }
    try { await this.control?.close() } catch { /* ignore */ }
    this.videoWriter = null
    this.audioWriter = null
    this.control = null
    try { this.wt?.close() } catch { /* ignore */ }
    this.wt = null
    this.tx('closed')
  }

  private async sendControl(buf: Uint8Array, name: string) {
    if (!this.control) throw new Error('control stream missing')
    await this.write(this.control, buf, `control ${name}`, true)
  }

  private async sendPublish(requestId: number, trackName: string, alias: number) {
    const buf = encodePublish({
      requestId,
      app: this.app,
      stream: this.stream,
      trackName,
      alias,
    })
    this.tx(
      `encode PUBLISH req=${requestId} ns=${this.app}/${this.stream} name=${trackName} alias=${alias} ${buf.byteLength}B`,
    )
    await this.sendControl(buf, `PUBLISH ${trackName}`)
  }

  private async sendCatalog(status: EncoderStatus) {
    const json = encodeCatalogJson({
      app: this.app,
      stream: this.stream,
      videoCodec: status.videoCodec,
      audioCodec: status.audioCodec,
      width: status.width,
      height: status.height,
    })
    const w = await this.openMediaBidi(`catalog alias=${ALIAS_CATALOG}`)
    const hdr = encodeSubgroupHeader(ALIAS_CATALOG, 0)
    const obj = encodeObject({ objectIdDelta: 0, timestampMs: 0, payload: json })
    await this.write(w, hdr, `SUBGROUP catalog alias=${ALIAS_CATALOG} group=0`)
    await this.write(w, obj, `OBJECT catalog ${json.byteLength}B`)
    this.tx(`catalog json=${new TextDecoder().decode(json)}`)
    try { await w.close() } catch { /* ignore */ }
    this.tx('catalog bidi closed')
  }

  private async sendVideo(payload: Uint8Array, dtsMs: number, key: boolean) {
    try {
      if (!this.videoWriter) {
        this.tx('skip video: no bidi writer')
        return
      }
      const cfg = !this.avcCSent && this.avcC ? this.avcC : undefined
      const obj = encodeObject({
        objectIdDelta: 0,
        timestampMs: dtsMs,
        payload,
        key,
        videoConfig: cfg,
      })
      if (cfg) this.avcCSent = true
      await this.write(
        this.videoWriter,
        obj,
        `OBJECT video #${this.videoObj} dts=${dtsMs} key=${key ? 1 : 0} ${payload.byteLength}B`,
        this.videoObj < 3,
      )
      this.stats.addVideo(payload.byteLength, dtsMs, key)
      this.videoObj += 1
    } catch (e) {
      this.closed = true
      this.tx(`ERROR video object: ${e instanceof Error ? e.message : String(e)}`, 'error')
    }
  }

  private async sendAudio(payload: Uint8Array, dtsMs: number) {
    try {
      if (!this.audioWriter) {
        if (this.audioObj === 0) this.tx('skip audio: no bidi writer')
        return
      }
      const cfg = !this.aacSent && this.aacAsc ? this.aacAsc : undefined
      const obj = encodeObject({
        objectIdDelta: 0,
        timestampMs: dtsMs,
        payload,
        audioConfig: cfg,
      })
      if (cfg) this.aacSent = true
      await this.write(
        this.audioWriter,
        obj,
        `OBJECT audio #${this.audioObj} dts=${dtsMs} ${payload.byteLength}B`,
        this.audioObj < 3,
      )
      this.stats.addAudio(payload.byteLength)
      this.audioObj += 1
    } catch (e) {
      this.closed = true
      this.tx(`ERROR audio object: ${e instanceof Error ? e.message : String(e)}`, 'error')
    }
  }

  /* 媒体暂用 bidi，与 FLV 推流同一路径；uni 以后再接 */
  private async openMediaBidi(label: string): Promise<WritableStreamDefaultWriter<Uint8Array>> {
    if (!this.wt) throw new Error('wt closed')
    this.tx(`bidi open begin ${label}`)
    const t0 = performance.now()
    try {
      const stream = await this.wt.createBidirectionalStream()
      try { void stream.readable.cancel() } catch { /* ignore */ }
      this.tx(`bidi open ok ${label} ${Math.round(performance.now() - t0)}ms`)
      return stream.writable.getWriter()
    } catch (e) {
      const msg = e instanceof Error ? e.message : String(e)
      this.tx(`bidi open FAIL ${label} ${msg}`, 'error')
      throw e
    }
  }

  private async write(
    writer: WritableStreamDefaultWriter<Uint8Array>,
    buf: Uint8Array,
    label: string,
    verbose = true,
  ) {
    if (verbose) {
      this.tx(`WRITE begin ${label} ${buf.byteLength}B desired=${writer.desiredSize} hex=${hex_preview(buf, 24)}`)
    }
    await writer.ready
    await writer.write(buf)
    if (verbose) {
      this.tx(`WRITE ok ${label} ${buf.byteLength}B desired=${writer.desiredSize}`)
    }
  }

  private startBeat() {
    this.stopBeat()
    this.beatTimer = window.setInterval(() => {
      this.tx(
        `beat sinkV=${this.sinkVideo} sinkA=${this.sinkAudio} txV=${this.videoObj} txA=${this.audioObj} ` +
          `vGroup=${this.videoGroup} closed=${this.closed ? 1 : 0} ` +
          `ctrl=${this.control ? 1 : 0} vW=${this.videoWriter ? 1 : 0} aW=${this.audioWriter ? 1 : 0} ` +
          `vPend=${this.videoPending} aPend=${this.audioPending} ` +
          `vLag=${this.lastVideoLagMs}/max${this.maxVideoLagMs} aLag=${this.lastAudioLagMs}/max${this.maxAudioLagMs}`,
      )
    }, 2000)
  }

  private stopBeat() {
    if (this.beatTimer) {
      window.clearInterval(this.beatTimer)
      this.beatTimer = 0
    }
  }

  private tx(msg: string, level: 'info' | 'error' | 'warn' = 'info') {
    if (level === 'error') log_error('moq-tx', msg)
    else if (level === 'warn') log_warn('moq-tx', msg)
    else log_info('moq-tx', msg)
    try { this.onTx?.(msg) } catch { /* ignore */ }
  }
}
