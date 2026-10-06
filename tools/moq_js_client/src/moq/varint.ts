/** MOQT 的整数编码 —— vi64。
 *
 * 2026-09-25 起线上用这套；此前是 QUIC 变长整数（RFC 9000 §16）。
 * 依据见 docs/moq-transport-gap.md 的 G1：draft-ietf-moq-transport-17 起 §1.4.1
 * 要求 MOQT 的变长整数「1 字节尽可能能装」且「能表示完整 uint64」，
 * QUIC 变长整数两条都不满足（首字节只剩 6 位，1 字节仅 0~63，上限 2^62-1）。
 * 两套编码【只有 0~63 一致】，所以服务端 / Web 端 / flv2moq_client 必须同时切换。
 *
 * 布局：长度 = 首字节前导 1 的个数 + 1，1~9 字节，9 字节形态首字节 0xFF。
 * 与 quic_packet 那套 quic_varint_* 无关 —— 那是 QUIC 层自用的，没变也不该变。
 *
 * 【非最小编码允许收】：draft-20 §1.4.1 明说 "Variable-length integers do not
 * need to be encoded using the minimum number of bytes"，Table 2 甚至把非最小的
 * 0x8025 = 37 当合法样例。我们发端只发最短是自身选择。
 */

/** vi64 编码。接受 number 或 bigint —— 超过 2^53 的值必须传 bigint。
 * 总是产出最短形态。 */
export function encodeVi64(v: number | bigint): Uint8Array {
  const big = typeof v === 'bigint' ? v : BigInt(Math.max(0, Math.floor(v)))
  let n = 1
  while (n <= 8 && big >= 1n << BigInt(7 * n)) n++
  const out = new Uint8Array(n)
  if (n <= 8) {
    const lead = n - 1
    const mask = lead === 0 ? 0 : (0xff << (8 - lead)) & 0xff
    const headBits = Number((big >> BigInt(8 * (n - 1))) & ((1n << BigInt(8 - n)) - 1n))
    out[0] = (mask | headBits) & 0xff
  } else {
    out[0] = 0xff
  }
  for (let i = 1; i < n; i++) {
    out[i] = Number((big >> BigInt(8 * (n - 1 - i))) & 0xffn)
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
  return concatBytes([encodeVi64(data.byteLength), data])
}

export function encodeUtf8(s: string): Uint8Array {
  return encodeBytes(new TextEncoder().encode(s))
}

/** vi64 解码。nonMinimal 表示对端用了非最短形态 —— 草案【允许】，
 * 只作提示，不要据此断连（见文件头）。
 *
 * 值超过 Number.MAX_SAFE_INTEGER 时返回 null：JS 的 number 装不下。
 * MOQT 里没有哪个字段会到这个量级（时间戳按微秒算到 2^53 是公元 2255 年），
 * 所以出现即视为对端异常/数据损坏，交给调用方既有的 "=== null ⇒ 数据不足"
 * 分支处理。这样比静默截断安全。需要精确大值请直接用 decodeVi64BigAt。 */
export function decodeVi64At(
  data: Uint8Array,
  off: number,
): { value: number; size: number; nonMinimal: boolean } | null {
  const r = decodeVi64BigAt(data, off)
  if (!r) return null
  if (r.value > BigInt(Number.MAX_SAFE_INTEGER)) return null
  return { value: Number(r.value), size: r.size, nonMinimal: r.nonMinimal }
}

/** 与 decodeVi64At 同，但值以 bigint 返回，不设上限。 */
export function decodeVi64BigAt(
  data: Uint8Array,
  off: number,
): { value: bigint; size: number; nonMinimal: boolean } | null {
  if (off >= data.byteLength) return null
  const first = data[off]
  let lead = 0
  while (lead < 8 && (first & (0x80 >> lead)) !== 0) lead++
  const n = lead + 1
  if (off + n > data.byteLength) return null
  let v = n <= 8 ? BigInt(first & ((1 << (8 - n)) - 1)) : 0n
  for (let i = 1; i < n; i++) v = (v << 8n) | BigInt(data[off + i])
  const nonMinimal = n >= 2 && v < (1n << BigInt(7 * (n - 1)))
  return { value: v, size: n, nonMinimal }
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

  readVi64(): number | null {
    const r = decodeVi64At(this.buf, 0)
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
