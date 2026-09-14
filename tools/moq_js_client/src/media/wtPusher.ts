import { log_debug, log_error, log_info, log_warn } from '../logger'

export class WtPusher {
  private wt: WebTransport | null = null
  private writer: WritableStreamDefaultWriter<Uint8Array> | null = null
  private queue: Uint8Array[] = []
  private sending = false
  private closed = false
  private queued = 0
  private readonly maxQueue = 2 * 1024 * 1024

  async connect(url: string, onClosed?: () => void) {
    this.closed = false
    log_info('wt-push', `connect ${url}`)
    this.wt = new WebTransport(url)
    await this.wt.ready
    log_info('wt-push', 'WebTransport ready')
    const stream = await this.wt.createBidirectionalStream()
    this.writer = stream.writable.getWriter()
    log_info('wt-push', 'bidi stream opened')
    this.wt.closed.catch((e) => {
      this.closed = true
      onClosed?.()
      log_warn('wt-push', `WebTransport closed: ${e instanceof Error ? e.message : String(e)}`)
    })
  }

  push(buf: Uint8Array) {
    if (this.closed || !this.writer) {
      log_warn('wt-push', `drop ${buf.byteLength} bytes, closed=${this.closed} writer=${!!this.writer}`)
      return
    }
    /* 队列满时丢掉最旧的 tag，不要丢掉当前大包（视频 5~8KB、音频 ~250B）。
     * 否则会只剩下音频，GOP 后半段没有 video。 */
    let evicted = 0
    while (this.queued + buf.byteLength > this.maxQueue && this.queue.length) {
      const head = this.queue[0]
      if (head.byteLength >= 3 && head[0] === 0x46 && head[1] === 0x4c && head[2] === 0x56) {
        break
      }
      this.queue.shift()
      this.queued -= head.byteLength
      evicted++
    }
    if (this.queued + buf.byteLength > this.maxQueue) {
      log_warn('wt-push', `queue overflow drop new ${buf.byteLength} queued=${this.queued}`)
      return
    }
    if (evicted) {
      log_warn('wt-push', `evict ${evicted} old tags to enqueue ${buf.byteLength} queued=${this.queued}`)
    }
    this.queue.push(buf)
    this.queued += buf.byteLength
    void this.drain()
  }

  private async drain() {
    if (this.sending) return
    this.sending = true
    try {
      while (this.queue.length && this.writer && !this.closed) {
        const buf = this.queue.shift()!
        this.queued -= buf.byteLength
        await this.writer.ready
        await this.writer.write(buf)
        log_debug('wt-push', `write ${buf.byteLength} remainQ=${this.queued}`)
      }
    } catch (e) {
      this.closed = true
      log_error('wt-push', `write failed: ${e instanceof Error ? e.message : String(e)}`)
    } finally {
      this.sending = false
    }
  }

  async close() {
    this.closed = true
    this.queue = []
    this.queued = 0
    try { await this.writer?.close() } catch { /* ignore */ }
    this.writer = null
    try { this.wt?.close() } catch { /* ignore */ }
    this.wt = null
  }
}
