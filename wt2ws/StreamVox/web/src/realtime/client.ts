import { int16ToBase64 } from '../audio/pcm'
import type { RealtimeTransport, ServerEvent, TransportKind } from './types'
import { WsTransport } from './ws'
import { WtTransport } from './wt'

export type { ServerEvent, TransportKind }

/**
 * Protocol-level client.  The transport (WS direct / WT via proxy) is chosen
 * when connecting; audio capture, playback and event handling are identical.
 */
export class RealtimeClient {
  onEvent: (event: ServerEvent) => void = () => {}
  onClose: (reason: string) => void = () => {}

  private transport: RealtimeTransport | null = null

  async connect(kind: TransportKind, url: string,
                session: Record<string, unknown>): Promise<void> {
    const transport: RealtimeTransport =
      kind === 'wt' ? new WtTransport() : new WsTransport()
    transport.onEvent = (event) => this.onEvent(event)
    transport.onClose = (reason) => {
      if (this.transport === transport) this.transport = null
      this.onClose(reason)
    }
    await transport.connect(url, session)
    this.transport = transport
  }

  sendAudio(pcm: Int16Array): void {
    this.sendJson({ type: 'input_audio_buffer.append', audio: int16ToBase64(pcm) })
  }

  commit(): void {
    this.sendJson({ type: 'input_audio_buffer.commit' })
    this.sendJson({ type: 'response.create' })
  }

  close(): void {
    this.transport?.close()
    this.transport = null
  }

  private sendJson(payload: Record<string, unknown>): void {
    this.transport?.send(JSON.stringify(payload))
  }
}
