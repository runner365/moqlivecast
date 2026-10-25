export type ServerEvent = { type: string; [key: string]: any }

export type TransportKind = 'ws' | 'wt'

/** Both transports carry the same JSON text messages. */
export interface RealtimeTransport {
  onEvent: (event: ServerEvent) => void
  onClose: (reason: string) => void
  connect(url: string, session: Record<string, unknown>): Promise<void>
  send(text: string): void
  close(): void
}
