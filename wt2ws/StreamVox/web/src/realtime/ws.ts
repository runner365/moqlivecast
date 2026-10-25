import type { RealtimeTransport, ServerEvent } from './types'

/** Direct WebSocket transport: browser -> StreamVox :8000/ws. */
export class WsTransport implements RealtimeTransport {
  onEvent: (event: ServerEvent) => void = () => {}
  onClose: (reason: string) => void = () => {}

  private ws: WebSocket | null = null
  private opened = false
  private closed = false

  connect(url: string, session: Record<string, unknown>): Promise<void> {
    return new Promise((resolve, reject) => {
      const ws = new WebSocket(url)
      this.ws = ws

      ws.onopen = () => {
        this.opened = true
        ws.send(JSON.stringify({ type: 'session.update', session }))
        resolve()
      }
      ws.onerror = () => {
        if (!this.opened) reject(new Error('WebSocket connect failed'))
      }
      ws.onmessage = (event) => {
        try {
          this.onEvent(JSON.parse(event.data))
        } catch {
          /* ignore non-JSON */
        }
      }
      ws.onclose = (event) => {
        this.ws = null
        this.finish(`WebSocket closed code=${event.code}${event.reason ? ` (${event.reason})` : ''}`)
      }
    })
  }

  send(text: string): void {
    if (this.ws && this.ws.readyState === WebSocket.OPEN) this.ws.send(text)
  }

  close(): void {
    try {
      this.ws?.close(1000, 'bye')
    } catch {
      /* already closed */
    }
    this.ws = null
    this.finish('closed by client')
  }

  private finish(reason: string): void {
    if (this.closed) return
    this.closed = true
    this.onClose(reason)
  }
}
