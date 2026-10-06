import {
  LOC_AUDIO_CONFIG,
  LOC_COMPOSITION_TIME,
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
  /** 解码时间戳 DTS（LOC TIMESTAMP 属性） */
  timestampMs: number
  /** pts - dts，缺省 0。有 B 帧时非零，播放端要靠它还原显示顺序 */
  ctsMs: number
  key: boolean
  videoConfig?: Uint8Array
  audioConfig?: Uint8Array
  payload: Uint8Array
}

export function tryReadControl(r: ByteReader): ControlMsg | null {
  const snap = r.snapshot()
  const type = r.readVi64()
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
  const requestId = r.readVi64()
  const alias = r.readVi64()
  if (requestId === null || alias === null) return null
  return { requestId, alias }
}

export function isSubscribeOk(msg: ControlMsg): boolean {
  return msg.type === MOQT_SUBSCRIBE_OK
}

export function tryReadSubgroup(r: ByteReader): { alias: number; hasProps: boolean } | null {
  const snap = r.snapshot()
  const flags = r.readVi64()
  const alias = r.readVi64()
  const group = r.readVi64()
  if (flags === null || alias === null || group === null) {
    r.restore(snap)
    return null
  }
  const idMode = (flags >> 1) & 0x03
  if (idMode === 0x02) {
    const sg = r.readVi64()
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
  const delta = r.readVi64()
  if (delta === null) {
    r.restore(snap)
    return null
  }
  let timestampMs = 0
  let ctsMs = 0
  let key = false
  let videoConfig: Uint8Array | undefined
  let audioConfig: Uint8Array | undefined
  if (hasProps) {
    const plen = r.readVi64()
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
      const dlt = pr.readVi64()
      if (dlt === null) break
      const type = prev + dlt
      prev = type
      if ((type & 1) === 0) {
        const val = pr.readVi64()
        if (val === null) break
        if (type === LOC_TIMESTAMP) timestampMs = val
        if (type === LOC_COMPOSITION_TIME) ctsMs = val
      } else {
        const vlen = pr.readVi64()
        if (vlen === null) break
        const raw = pr.readBytes(vlen)
        if (!raw) break
        if (type === LOC_FRAME_MARK && raw.byteLength > 0 && (raw[0] & 0x01)) key = true
        if (type === LOC_VIDEO_CONFIG) videoConfig = raw.slice()
        if (type === LOC_AUDIO_CONFIG) audioConfig = raw.slice()
      }
    }
  }
  const payloadLen = r.readVi64()
  if (payloadLen === null) {
    r.restore(snap)
    return null
  }
  if (payloadLen === 0) {
    const status = r.readVi64()
    if (status === null) {
      r.restore(snap)
      return null
    }
    /* 注意：payload 为 0 不代表没有 config —— 服务端的 config 对象可能只带属性不带 payload，
     * 丢掉 videoConfig/audioConfig 会让解码器永远配不起来。 */
    return { timestampMs, ctsMs, key, videoConfig, audioConfig, payload: new Uint8Array(0) }
  }
  const payload = r.readBytes(payloadLen)
  if (!payload) {
    r.restore(snap)
    return null
  }
  return { timestampMs, ctsMs, key, videoConfig, audioConfig, payload: payload.slice() }
}
