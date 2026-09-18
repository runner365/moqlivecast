import { concatBytes, encodeU16, encodeUtf8, encodeVarint } from './varint'

/** draft-ietf-moq-transport */
export const MOQT_SETUP = 0x2f00
export const MOQT_PUBLISH = 0x1d
export const MOQT_SUBSCRIBE = 0x03
export const MOQT_SUBSCRIBE_OK = 0x04

/** LOC property IDs, draft-ietf-moq-loc */
export const LOC_TIMESCALE = 0x08
export const LOC_FRAME_MARK = 0x09
export const LOC_VIDEO_CONFIG = 0x0d
/** 私有扩展：CompositionTime = pts - dts，单位同 Timescale（毫秒）。
 *  有 B 帧时非零；缺省即 0，等价于 pts == dts。与服务端 moq_push.cpp 的
 *  kLocCompositionTime 对应。 */
export const LOC_COMPOSITION_TIME = 0x0e
export const LOC_AUDIO_CONFIG = 0x0f
export const LOC_TIMESTAMP = 0x10

export const ALIAS_CATALOG = 0
export const ALIAS_VIDEO = 1
export const ALIAS_AUDIO = 2

const SUBGROUP_FLAGS = 0x31 /* PROPERTIES | bit4 | DEFAULT_PRIORITY */

function encodeKvp(prevType: number, type: number, value: Uint8Array | number): {
  bytes: Uint8Array
  type: number
} {
  const delta = type - prevType
  const head = encodeVarint(delta)
  if ((type & 1) === 0) {
    const v = typeof value === 'number' ? encodeVarint(value) : encodeVarint(0)
    return { bytes: concatBytes([head, v]), type }
  }
  const raw = typeof value === 'number' ? encodeVarint(value) : value
  return { bytes: concatBytes([head, encodeVarint(raw.byteLength), raw]), type }
}

export function encodeProperties(items: Array<{ type: number; value: Uint8Array | number }>): Uint8Array {
  const sorted = [...items].sort((a, b) => a.type - b.type)
  const parts: Uint8Array[] = []
  let prev = 0
  for (const it of sorted) {
    const k = encodeKvp(prev, it.type, it.value)
    parts.push(k.bytes)
    prev = k.type
  }
  const body = concatBytes(parts)
  return concatBytes([encodeVarint(body.byteLength), body])
}

export function encodeSetup(): Uint8Array {
  return concatBytes([encodeVarint(MOQT_SETUP), encodeU16(0)])
}

export function encodeNamespace(fields: string[]): Uint8Array {
  const parts: Uint8Array[] = [encodeVarint(fields.length)]
  for (const f of fields) parts.push(encodeUtf8(f))
  return concatBytes(parts)
}

export function encodePublish(opts: {
  requestId: number
  app: string
  stream: string
  trackName: string
  alias: number
}): Uint8Array {
  const name = new TextEncoder().encode(opts.trackName)
  const body = concatBytes([
    encodeVarint(opts.requestId),
    encodeNamespace([opts.app, opts.stream]),
    encodeVarint(name.byteLength),
    name,
    encodeVarint(opts.alias),
    encodeVarint(0),
    encodeVarint(0),
  ])
  return concatBytes([encodeVarint(MOQT_PUBLISH), encodeU16(body.byteLength), body])
}

/** 与 PUBLISH 同布局：req + ns[app,stream] + name + alias */
export function encodeSubscribe(opts: {
  requestId: number
  app: string
  stream: string
  trackName: string
  alias: number
}): Uint8Array {
  const name = new TextEncoder().encode(opts.trackName)
  const body = concatBytes([
    encodeVarint(opts.requestId),
    encodeNamespace([opts.app, opts.stream]),
    encodeVarint(name.byteLength),
    name,
    encodeVarint(opts.alias),
  ])
  return concatBytes([encodeVarint(MOQT_SUBSCRIBE), encodeU16(body.byteLength), body])
}

export function encodeSubgroupHeader(alias: number, groupId: number): Uint8Array {
  return concatBytes([
    encodeVarint(SUBGROUP_FLAGS),
    encodeVarint(alias),
    encodeVarint(groupId),
  ])
}

export function encodeObject(opts: {
  objectIdDelta: number
  timestampMs: number
  payload: Uint8Array
  key?: boolean
  videoConfig?: Uint8Array
  audioConfig?: Uint8Array
}): Uint8Array {
  const props: Array<{ type: number; value: Uint8Array | number }> = [
    { type: LOC_TIMESCALE, value: 1000 },
    { type: LOC_TIMESTAMP, value: opts.timestampMs },
  ]
  if (opts.key) {
    props.push({ type: LOC_FRAME_MARK, value: new Uint8Array([0x01]) })
  }
  if (opts.videoConfig) {
    props.push({ type: LOC_VIDEO_CONFIG, value: opts.videoConfig })
  }
  if (opts.audioConfig) {
    props.push({ type: LOC_AUDIO_CONFIG, value: opts.audioConfig })
  }
  return concatBytes([
    encodeVarint(opts.objectIdDelta),
    encodeProperties(props),
    encodeVarint(opts.payload.byteLength),
    opts.payload,
  ])
}

export function encodeCatalogJson(opts: {
  app: string
  stream: string
  videoCodec: string
  audioCodec: string
  width: number
  height: number
}): Uint8Array {
  const ns = `${opts.app}/${opts.stream}`
  const json = JSON.stringify({
    tracks: [
      {
        name: 'video',
        namespace: ns,
        codec: opts.videoCodec || 'avc1.64001f',
        packaging: 'loc',
        width: opts.width,
        height: opts.height,
      },
      {
        name: 'audio',
        namespace: ns,
        codec: opts.audioCodec || 'mp4a.40.2',
        packaging: 'loc',
        samplerate: 48000,
        channelConfig: 'stereo',
      },
    ],
  })
  return new TextEncoder().encode(json)
}
