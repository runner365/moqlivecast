import {
  LOC_AUDIO_CONFIG,
  LOC_FRAME_MARK,
  LOC_TIMESTAMP,
  LOC_VIDEO_CONFIG,
  MOQT_SUBSCRIBE_OK,
} from './wire'
import { ByteReader } from './varint'

export type ControlMsg = {
  type: number
  body: Uint8Array
}

export type LocObject = {
  timestampMs: number
  key: boolean
  videoConfig?: Uint8Array
  audioConfig?: Uint8Array
  payload: Uint8Array
}

export function tryReadControl(r: ByteReader): ControlMsg | null {
  const snap = r.snapshot()
  const type = r.readVarint()
  const len = r.readU16()
  if (type === null || len === null) {
    r.restore(snap)
    return null
  }
  const body = r.readBytes(len)
  if (!body) {
    r.restore(snap)
    return null
  }
  return { type, body: body.slice() }
}

export function parseSubscribeOk(body: Uint8Array): { requestId: number; alias: number } | null {
  const r = new ByteReader()
  r.push(body)
  const requestId = r.readVarint()
  const alias = r.readVarint()
  if (requestId === null || alias === null) return null
  return { requestId, alias }
}

export function isSubscribeOk(msg: ControlMsg): boolean {
  return msg.type === MOQT_SUBSCRIBE_OK
}

export function tryReadSubgroup(r: ByteReader): { alias: number; hasProps: boolean } | null {
  const snap = r.snapshot()
  const flags = r.readVarint()
  const alias = r.readVarint()
  const group = r.readVarint()
  if (flags === null || alias === null || group === null) {
    r.restore(snap)
    return null
  }
  const idMode = (flags >> 1) & 0x03
  if (idMode === 0x02) {
    const sg = r.readVarint()
    if (sg === null) {
      r.restore(snap)
      return null
    }
  }
  if ((flags & 0x20) === 0) {
    const pri = r.readBytes(1)
    if (!pri) {
      r.restore(snap)
      return null
    }
  }
  return { alias, hasProps: (flags & 0x01) !== 0 }
}

export function tryReadObject(r: ByteReader, hasProps: boolean): LocObject | null {
  const snap = r.snapshot()
  const delta = r.readVarint()
  if (delta === null) {
    r.restore(snap)
    return null
  }
  let timestampMs = 0
  let key = false
  let videoConfig: Uint8Array | undefined
  let audioConfig: Uint8Array | undefined
  if (hasProps) {
    const plen = r.readVarint()
    if (plen === null) {
      r.restore(snap)
      return null
    }
    const props = r.readBytes(plen)
    if (!props) {
      r.restore(snap)
      return null
    }
    const pr = new ByteReader()
    pr.push(props)
    let prev = 0
    while (pr.length > 0) {
      const dlt = pr.readVarint()
      if (dlt === null) break
      const type = prev + dlt
      prev = type
      if ((type & 1) === 0) {
        const val = pr.readVarint()
        if (val === null) break
        if (type === LOC_TIMESTAMP) timestampMs = val
      } else {
        const vlen = pr.readVarint()
        if (vlen === null) break
        const raw = pr.readBytes(vlen)
        if (!raw) break
        if (type === LOC_FRAME_MARK && raw.byteLength > 0 && (raw[0] & 0x01)) key = true
        if (type === LOC_VIDEO_CONFIG) videoConfig = raw.slice()
        if (type === LOC_AUDIO_CONFIG) audioConfig = raw.slice()
      }
    }
  }
  const payloadLen = r.readVarint()
  if (payloadLen === null) {
    r.restore(snap)
    return null
  }
  if (payloadLen === 0) {
    const status = r.readVarint()
    if (status === null) {
      r.restore(snap)
      return null
    }
    /* 注意：payload 为 0 不代表没有 config —— 服务端的 config 对象可能只带属性不带 payload，
     * 丢掉 videoConfig/audioConfig 会让解码器永远配不起来。 */
    return { timestampMs, key, videoConfig, audioConfig, payload: new Uint8Array(0) }
  }
  const payload = r.readBytes(payloadLen)
  if (!payload) {
    r.restore(snap)
    return null
  }
  return { timestampMs, key, videoConfig, audioConfig, payload: payload.slice() }
}
