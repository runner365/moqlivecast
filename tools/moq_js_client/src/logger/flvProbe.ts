import { hex_preview, log_debug, log_info, log_warn } from './logger'

const TAG_NAME: Record<number, string> = {
  8: 'audio',
  9: 'video',
  18: 'script',
}

function concatU8(a: Uint8Array, b: Uint8Array): Uint8Array {
  const out = new Uint8Array(a.byteLength + b.byteLength)
  out.set(a, 0)
  out.set(b, a.byteLength)
  return out
}

function copyU8(view: Uint8Array): Uint8Array {
  return new Uint8Array(view)
}

function u24(d: Uint8Array, off: number): number {
  return (d[off] << 16) | (d[off + 1] << 8) | d[off + 2]
}

/* FLV tag：off 指向 timestamp 起始（tag 内偏移 4）
 * [4..6]=ts 低 24bit，[7]=ts 高 8bit；stream id 在 [8..10] 恒为 0 */
function tagTs(d: Uint8Array, off: number): number {
  return (d[off] << 16) | (d[off + 1] << 8) | d[off + 2] | (d[off + 3] << 24)
}

function describeBody(type: number, body0: number, body1: number): string {
  if (type === 9) {
    const key = (body0 & 0xf0) === 0x10 ? 'key' : 'inter'
    const pkt = body1 === 0x00 ? 'seq' : body1 === 0x01 ? 'nalu' : `pt=0x${body1.toString(16)}`
    return `${key}/${pkt} codec=0x${(body0 & 0x0f).toString(16)}`
  }
  if (type === 8) {
    const pkt = body1 === 0x00 ? 'seq' : body1 === 0x01 ? 'raw' : `pt=0x${body1.toString(16)}`
    return `${pkt} fmt=0x${(body0 >> 4).toString(16)}`
  }
  return ''
}

/** 把下行 FLV 字节解析成 header/tag，供控制台定位 */
export class FlvLogProbe {
  private buf: Uint8Array = new Uint8Array(0)
  private headerDone = false
  private tags = 0
  private warnedBadMagic = false

  reset() {
    this.buf = new Uint8Array(0)
    this.headerDone = false
    this.tags = 0
    this.warnedBadMagic = false
  }

  push(chunk: Uint8Array) {
    if (chunk.byteLength === 0) return
    this.buf = concatU8(this.buf, copyU8(chunk))

    if (!this.headerDone) {
      if (this.buf.byteLength < 13) return
      const magic = String.fromCharCode(this.buf[0], this.buf[1], this.buf[2])
      const ver = this.buf[3]
      const flags = this.buf[4]
      const offset = (this.buf[5] << 24) | (this.buf[6] << 16) | (this.buf[7] << 8) | this.buf[8]
      if (magic !== 'FLV') {
        if (!this.warnedBadMagic) {
          this.warnedBadMagic = true
          log_warn(
            'flv-probe',
            `首包不是 FLV header magic=${JSON.stringify(magic)} hex=${hex_preview(this.buf, 16)}`,
          )
        }
        if (this.buf.byteLength > 64 * 1024) {
          this.buf = copyU8(this.buf.subarray(this.buf.byteLength - 4096))
        }
        return
      }
      log_info(
        'flv-probe',
        `FLV header ver=${ver} flags=0x${flags.toString(16)} ` +
          `hasVideo=${(flags & 0x01) !== 0} hasAudio=${(flags & 0x04) !== 0} offset=${offset}`,
      )
      this.buf = copyU8(this.buf.subarray(13))
      this.headerDone = true
    }

    while (this.buf.byteLength >= 11) {
      const type = this.buf[0]
      const size = u24(this.buf, 1)
      const need = 11 + size + 4
      if (this.buf.byteLength < need) break

      const dts = tagTs(this.buf, 4)
      const body0 = size > 0 ? this.buf[11] : 0
      const body1 = size > 1 ? this.buf[12] : 0
      this.tags++
      const name = TAG_NAME[type] ?? `0x${type.toString(16)}`
      const extra = describeBody(type, body0, body1)
      log_debug(
        'flv-probe',
        `tag#${this.tags} type=${name} ts=${dts} size=${size}` +
          (extra ? ` ${extra}` : ''),
      )

      this.buf = copyU8(this.buf.subarray(need))
    }
  }
}
