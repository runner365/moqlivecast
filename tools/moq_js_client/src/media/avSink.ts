export interface AvSink {
  writeAvcSeq(avcC: Uint8Array, dtsMs: number): void
  writeAvcNalu(payload: Uint8Array, dtsMs: number, ctsMs: number, key: boolean): void
  writeAacSeq(asc: Uint8Array, dtsMs: number): void
  writeAacRaw(payload: Uint8Array, dtsMs: number): void
}
