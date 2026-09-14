/** FLV live muxer：H.264 (AVC) + AAC → FLV tags */

import { log_debug, log_info } from '../logger'

function u8(...bytes: number[]): Uint8Array {
  return new Uint8Array(bytes)
}

function concat(parts: Uint8Array[]): Uint8Array {
  let n = 0
  for (const p of parts) n += p.byteLength
  const out = new Uint8Array(n)
  let off = 0
  for (const p of parts) {
    out.set(p, off)
    off += p.byteLength
  }
  return out
}

function writeU24(n: number): Uint8Array {
  return u8((n >>> 16) & 0xff, (n >>> 8) & 0xff, n & 0xff)
}

function writeU32(n: number): Uint8Array {
  return u8((n >>> 24) & 0xff, (n >>> 16) & 0xff, (n >>> 8) & 0xff, n & 0xff)
}

function tsFields(dtsMs: number): Uint8Array {
  const base = dtsMs & 0xffffff
  const ext = (dtsMs >>> 24) & 0xff
  return u8((base >>> 16) & 0xff, (base >>> 8) & 0xff, base & 0xff, ext)
}

/** AAC-LC / 48 kHz / stereo AudioSpecificConfig */
export function aacLc48kStereoAsc(): Uint8Array {
  /* object=2, freqIdx=3 (48k), channels=2 → 0x11 0x90 */
  return u8(0x11, 0x90)
}

export function stripAdts(data: Uint8Array): Uint8Array {
  if (data.byteLength >= 7 && data[0] === 0xff && (data[1] & 0xf0) === 0xf0) {
    const hdr = (data[1] & 1) !== 0 ? 7 : 9
    return data.subarray(hdr)
  }
  return data
}

export type FlvSink = (buf: Uint8Array) => void

export class FlvMuxer {
  private headerSent = false
  private avcSeqSent = false
  private aacSeqSent = false
  private audioTags = 0
  private videoTags = 0
  private sink: FlvSink

  constructor(sink: FlvSink) {
    this.sink = sink
  }

  private emitTag(tagType: number, dtsMs: number, body: Uint8Array) {
    const header = concat([
      u8(tagType),
      writeU24(body.byteLength),
      tsFields(dtsMs),
      u8(0, 0, 0),
    ])
    const prev = writeU32(11 + body.byteLength)
    this.sink(concat([header, body, prev]))
  }

  ensureHeader() {
    if (this.headerSent) return
    this.headerSent = true
    log_info('flv-mux', 'write FLV header flags=0x05')
    /* FLV + ver1 + audio|video + dataOffset=9 + prevTagSize0 */
    this.sink(u8(
      0x46, 0x4c, 0x56, 0x01, 0x05,
      0x00, 0x00, 0x00, 0x09,
      0x00, 0x00, 0x00, 0x00,
    ))
  }

  writeAvcSeq(avcC: Uint8Array, dtsMs: number) {
    this.ensureHeader()
    if (this.avcSeqSent) return
    this.avcSeqSent = true
    log_info('flv-mux', `write AVC seq dts=${dtsMs} avcC=${avcC.byteLength}B`)
    /* key + AVC, packetType=seq, cts=0 */
    const body = concat([u8(0x17, 0x00, 0x00, 0x00, 0x00), avcC])
    this.emitTag(0x09, dtsMs, body)
  }

  writeAvcNalu(payload: Uint8Array, dtsMs: number, ctsMs: number, key: boolean) {
    this.ensureHeader()
    const frame = key ? 0x17 : 0x27
    const cts = Math.max(0, ctsMs | 0)
    const body = concat([
      u8(frame, 0x01, (cts >>> 16) & 0xff, (cts >>> 8) & 0xff, cts & 0xff),
      payload,
    ])
    this.videoTags++
    log_debug(
      'flv-mux',
      `AVC ${key ? 'key' : 'p'} #${this.videoTags} dts=${dtsMs} size=${payload.byteLength} a=${this.audioTags}`,
    )
    this.emitTag(0x09, dtsMs, body)
  }

  writeAacSeq(asc: Uint8Array, dtsMs: number) {
    this.ensureHeader()
    if (this.aacSeqSent) return
    this.aacSeqSent = true
    log_info('flv-mux', `write AAC seq dts=${dtsMs} asc=${asc.byteLength}B`)
    /* AAC 16bit stereo, packetType=seq（soundRate 对 AAC 无效，真实采样率在 ASC） */
    const body = concat([u8(0xaf, 0x00), asc])
    this.emitTag(0x08, dtsMs, body)
  }

  writeAacRaw(payload: Uint8Array, dtsMs: number) {
    this.ensureHeader()
    const raw = stripAdts(payload)
    const body = concat([u8(0xaf, 0x01), raw])
    this.audioTags++
    log_debug('flv-mux', `AAC raw #${this.audioTags} dts=${dtsMs} size=${raw.byteLength}`)
    this.emitTag(0x08, dtsMs, body)
  }

  get ready() {
    return this.headerSent
  }
}
