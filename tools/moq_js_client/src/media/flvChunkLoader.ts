/* 把 WebTransport 读到的 FLV 字节喂给 flv.js 的自定义 Loader。
 * 必须等 IOController 调 open() 并挂好 onDataArrival 后再 dispatch，
 * 否则首包 byteStart>0 时 _demuxer 仍为 null，会炸 bindDataSource。 */

import { hex_preview, log_debug, log_info, log_warn } from '../logger'

export class FlvChunkSource {
  private loader: FlvChunkLoader | null = null
  private pending: Uint8Array[] = []

  attach(loader: FlvChunkLoader) {
    this.loader = loader
  }

  detach() {
    this.loader = null
  }

  flushPending() {
    if (!this.loader?.isOpen() || this.pending.length === 0) return
    log_info('flv-loader', `flush pending ${this.pending.length} chunks`)
    const chunks = this.pending
    this.pending = []
    for (const chunk of chunks) {
      this.loader.dispatch(chunk)
    }
  }

  push(data: Uint8Array) {
    const copy = data.slice()
    if (this.loader?.isOpen()) {
      this.loader.dispatch(copy)
      return
    }
    this.pending.push(copy)
    if (this.pending.length <= 4) {
      log_info(
        'flv-loader',
        `stash before open len=${copy.byteLength} pending=${this.pending.length} hex=${hex_preview(copy)}`,
      )
    } else {
      log_debug('flv-loader', `stash before open len=${copy.byteLength} pending=${this.pending.length}`)
    }
  }
}

let boundSource: FlvChunkSource | null = null

export function bindFlvChunkSource(source: FlvChunkSource | null) {
  boundSource = source
}

/* flv.js IOController：new customLoader → 赋 onDataArrival → open() */
export class FlvChunkLoader {
  _status = 0
  _needStash = false
  extraData: unknown = null
  onContentLengthKnown: (contentLength: number) => void = () => {}
  onURLRedirect: (redirectedURL: string) => void = () => {}
  onDataArrival: (chunk: ArrayBuffer, byteStart: number, receivedLength?: number) => void = () => {}
  onError: (errorType: unknown, errorInfo: unknown) => void = () => {}
  onComplete: (rangeFrom: number, rangeTo: number) => void = () => {}

  private byteStart_ = 0
  private source_: FlvChunkSource | null = null

  constructor(_seekHandler?: unknown, _config?: unknown) {
    this.source_ = boundSource
    log_info('flv-loader', `construct bound=${!!this.source_}`)
    this.source_?.attach(this)
  }

  get status() {
    return this._status
  }

  get type() {
    return 'flv-chunk-loader'
  }

  get needStashBuffer() {
    return this._needStash
  }

  isOpen() {
    return this._status === 2
  }

  destroy() {
    this.source_?.detach()
    this.source_ = null
    this._status = 3
    this.onDataArrival = () => {}
  }

  isWorking() {
    return this._status === 2
  }

  open(_dataSource: unknown, _range: unknown) {
    this._status = 2
    this.byteStart_ = 0
    log_info('flv-loader', 'open')
    this.source_?.flushPending()
  }

  abort() {
    this._status = 3
  }

  dispatch(u8: Uint8Array) {
    if (u8.byteLength === 0) return
    if (this._status !== 2) {
      log_warn('flv-loader', `drop ${u8.byteLength}B, loader not open`)
      return
    }
    const copy = new Uint8Array(u8)
    const from = this.byteStart_
    this.byteStart_ += copy.byteLength
    if (from === 0) {
      log_info(
        'flv-loader',
        `feed flv.js first chunk off=0 len=${copy.byteLength} hex=${hex_preview(copy)}`,
      )
    } else if (this.byteStart_ < 64 * 1024 || this.byteStart_ % (256 * 1024) < copy.byteLength) {
      log_debug('flv-loader', `feed flv.js off=${from} len=${copy.byteLength} total=${this.byteStart_}`)
    }
    this.onDataArrival(copy.buffer, from, this.byteStart_)
  }
}
