export type FlvMediaKind = 'video' | 'audio'

export class FlvDemuxer {
  constructor(
    mod?: string,
    onMedia?: (kind: FlvMediaKind, size: number, ts: number, key: boolean) => void,
  )
  reset(): void
  push(chunk: Uint8Array): void
}
