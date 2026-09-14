/** QUIC / MOQT variable-length integer (RFC 9000). */

export function encodeVarint(v: number): Uint8Array {
  const n = Math.max(0, Math.floor(v))
  if (n < 64) return new Uint8Array([n])
  if (n < 16384) return new Uint8Array([0x40 | (n >> 8), n & 0xff])
  if (n < 2 ** 30) {
    return new Uint8Array([
      0x80 | ((n >>> 24) & 0x3f),
      (n >>> 16) & 0xff,
      (n >>> 8) & 0xff,
      n & 0xff,
    ])
  }
  const big = BigInt(n)
  const out = new Uint8Array(8)
  out[0] = 0xc0 | Number((big >> 56n) & 0x3fn)
  for (let i = 1; i < 8; i++) {
    out[i] = Number((big >> BigInt((7 - i) * 8)) & 0xffn)
  }
  return out
}

export function concatBytes(parts: Uint8Array[]): Uint8Array {
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

export function encodeU16(n: number): Uint8Array {
  return new Uint8Array([(n >>> 8) & 0xff, n & 0xff])
}

export function encodeBytes(data: Uint8Array): Uint8Array {
  return concatBytes([encodeVarint(data.byteLength), data])
}

export function encodeUtf8(s: string): Uint8Array {
  return encodeBytes(new TextEncoder().encode(s))
}

export function decodeVarintAt(data: Uint8Array, off: number): { value: number; size: number } | null {
  if (off >= data.byteLength) return null
  const n = 1 << (data[off] >> 6)
  if (off + n > data.byteLength) return null
  let v = data[off] & 0x3f
  for (let i = 1; i < n; i++) v = (v << 8) | data[off + i]
  return { value: v, size: n }
}

export class ByteReader {
  private buf: Uint8Array = new Uint8Array(0)

  push(chunk: Uint8Array) {
    if (!chunk.byteLength) return
    const n = new Uint8Array(this.buf.byteLength + chunk.byteLength)
    n.set(this.buf)
    n.set(chunk, this.buf.byteLength)
    this.buf = n
  }

  get length() {
    return this.buf.byteLength
  }

  snapshot(): Uint8Array {
    return this.buf
  }

  restore(buf: Uint8Array) {
    this.buf = buf
  }

  readVarint(): number | null {
    const r = decodeVarintAt(this.buf, 0)
    if (!r) return null
    this.buf = this.buf.subarray(r.size)
    return r.value
  }

  readU16(): number | null {
    if (this.buf.byteLength < 2) return null
    const v = (this.buf[0] << 8) | this.buf[1]
    this.buf = this.buf.subarray(2)
    return v
  }

  readBytes(n: number): Uint8Array | null {
    if (n < 0 || this.buf.byteLength < n) return null
    const v = this.buf.subarray(0, n)
    this.buf = this.buf.subarray(n)
    return v
  }
}
