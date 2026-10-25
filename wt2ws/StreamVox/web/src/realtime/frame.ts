const HEADER = 5
const FRAME_TEXT = 1
const MAX_FRAME = 16 * 1024 * 1024

/** [u32 BE payload length][u8 type][payload] — matches wt2ws_proxy. */
export function encodeTextFrame(text: string): Uint8Array {
  const payload = new TextEncoder().encode(text)
  const buf = new Uint8Array(HEADER + payload.length)
  new DataView(buf.buffer).setUint32(0, payload.length, false)
  buf[4] = FRAME_TEXT
  buf.set(payload, HEADER)
  return buf
}

/** Parse every complete frame, returning the unconsumed tail. */
export function decodeFrames(
  input: Uint8Array,
  onText: (text: string) => void,
  onError: (message: string) => void,
): Uint8Array {
  let buf = input
  while (buf.length >= HEADER) {
    const len = new DataView(buf.buffer, buf.byteOffset, HEADER).getUint32(0, false)
    if (len > MAX_FRAME) {
      onError(`frame too large: ${len}`)
      return new Uint8Array(0)
    }
    if (buf.length < HEADER + len) break
    const type = buf[4]
    const payload = buf.slice(HEADER, HEADER + len)
    buf = buf.slice(HEADER + len)
    if (type === FRAME_TEXT) {
      onText(new TextDecoder().decode(payload))
    } else {
      onError(`unknown frame type ${type}`)
      return new Uint8Array(0)
    }
  }
  return buf
}
