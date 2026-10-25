import { decodeFrames, encodeTextFrame } from './frame'
import type { RealtimeTransport, ServerEvent } from './types'

/**
 * WebTransport transport: browser -> wt2ws_proxy :5433 -> StreamVox :8000/ws.
 *
 * One client-initiated bidirectional stream carries every JSON message, framed
 * as [u32 BE length][u8 type=1][UTF-8 payload].
 */
export class WtTransport implements RealtimeTransport {
  onEvent: (event: ServerEvent) => void = () => {}
  onClose: (reason: string) => void = () => {}

  private wt: any = null
  private writer: WritableStreamDefaultWriter<Uint8Array> | null = null
  private reader: ReadableStreamDefaultReader<Uint8Array> | null = null
  private pending: Uint8Array = new Uint8Array(0)
  private closed = false
  private heartbeat: number | null = null

  async connect(url: string, session: Record<string, unknown>): Promise<void> {
    const Ctor = (window as any).WebTransport
    if (!Ctor) throw new Error('this browser does not support WebTransport')

    const wt = new Ctor(url)
    this.wt = wt
    await wt.ready

    const stream = await wt.createBidirectionalStream()
    this.writer = stream.writable.getWriter()
    this.reader = stream.readable.getReader()

    wt.closed
      .then(() => this.finish('WebTransport session closed'))
      .catch((e: any) => this.finish(e?.message ? `WebTransport: ${e.message}` : 'WebTransport error'))

    void this.readLoop()
    this.send(JSON.stringify({ type: 'session.update', session }))

    // QUIC has its own keepalive, but wt2ws_proxy also has an application idle
    // timeout (default 5 min).  A tiny JSON ping resets it and gets a pong.
    this.heartbeat = window.setInterval(() => {
      this.send(JSON.stringify({ type: 'ping' }))
    }, 15000)
  }

  send(text: string): void {
    if (!this.writer || this.closed) return
    const frame = encodeTextFrame(text)
    // Copy into a fresh ArrayBuffer-backed view; TS 5.9 distinguishes
    // Uint8Array<ArrayBufferLike> from the WritableStream's Uint8Array<ArrayBuffer>.
    const ab = new ArrayBuffer(frame.length)
    const copy = new Uint8Array(ab)
    copy.set(frame)
    this.writer.write(copy as Uint8Array<ArrayBuffer>).catch((e: any) => {
      this.finish(e?.message ? `write failed: ${e.message}` : 'write failed')
    })
  }

  close(): void {
    try {
      this.wt?.close()
    } catch {
      /* already closed */
    }
    this.finish('closed by client')
  }

  private async readLoop(): Promise<void> {
    try {
      for (;;) {
        const { value, done } = await this.reader!.read()
        if (done) break
        if (!value) continue
        const merged = new Uint8Array(this.pending.length + value.length)
        merged.set(this.pending, 0)
        merged.set(value, this.pending.length)
        this.pending = decodeFrames(
          merged,
          (text) => {
            try {
              this.onEvent(JSON.parse(text))
            } catch {
              /* ignore non-JSON */
            }
          },
          (message) => this.finish(message),
        )
        if (this.closed) return
      }
    } catch (e: any) {
      this.finish(e?.message ? `read error: ${e.message}` : 'read error')
      return
    }
    this.finish('stream ended')
  }

  private finish(reason: string): void {
    if (this.closed) return
    this.closed = true
    if (this.heartbeat !== null) {
      window.clearInterval(this.heartbeat)
      this.heartbeat = null
    }
    try {
      this.reader?.cancel()
    } catch {
      /* ignore */
    }
    this.onClose(reason)
  }
}
