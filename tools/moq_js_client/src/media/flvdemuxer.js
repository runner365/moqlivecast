/**
 * 拉流侧 FLV 解封装，只打日志，不改数据、不回调播放器。
 * 用来核对下行 header / tag / AVC / AAC 是否完整、时间戳是否连续。
 */

import { hex_preview, log_debug, log_info, log_warn } from '../logger'

const TAG_NAME = { 8: 'audio', 9: 'video', 18: 'script' }
const AVC_PKT = { 0: 'seq', 1: 'nalu', 2: 'end' }
const AAC_PKT = { 0: 'seq', 1: 'raw' }
const NAL_NAME = {
  1: 'non-IDR',
  5: 'IDR',
  6: 'SEI',
  7: 'SPS',
  8: 'PPS',
  9: 'AUD',
}
const AAC_FREQ = [
  96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350,
]

function concatU8(a, b) {
  const out = new Uint8Array(a.byteLength + b.byteLength)
  out.set(a, 0)
  out.set(b, a.byteLength)
  return out
}

function copyU8(view) {
  return new Uint8Array(view)
}

function u16(d, off) {
  return (d[off] << 8) | d[off + 1]
}

function u24(d, off) {
  return (d[off] << 16) | (d[off + 1] << 8) | d[off + 2]
}

function u32(d, off) {
  return ((d[off] << 24) | (d[off + 1] << 16) | (d[off + 2] << 8) | d[off + 3]) >>> 0
}

/** FLV tag ts：[4..6] 低 24bit，[7] 高 8bit */
function tagTs(d, off) {
  return (d[off] << 16) | (d[off + 1] << 8) | d[off + 2] | (d[off + 3] << 24)
}

function nalName(t) {
  return NAL_NAME[t] || `nal=${t}`
}

function parseAvcC(avcC) {
  if (avcC.byteLength < 7) return `avcC too short ${avcC.byteLength}B hex=${hex_preview(avcC)}`
  const ver = avcC[0]
  const profile = avcC[1]
  const compat = avcC[2]
  const level = avcC[3]
  const naluLen = (avcC[4] & 3) + 1
  let off = 5
  const spsN = avcC[off] & 0x1f
  off += 1
  const sps = []
  for (let i = 0; i < spsN && off + 2 <= avcC.byteLength; i++) {
    const n = u16(avcC, off)
    off += 2
    if (off + n > avcC.byteLength) {
      sps.push(`truncated(${n})`)
      break
    }
    sps.push(`${n}B nal=${avcC[off] & 0x1f}`)
    off += n
  }
  if (off >= avcC.byteLength) {
    return `ver=${ver} profile=0x${profile.toString(16)} level=${level} naluLen=${naluLen} sps=[${sps}] pps=missing`
  }
  const ppsN = avcC[off]
  off += 1
  const pps = []
  for (let i = 0; i < ppsN && off + 2 <= avcC.byteLength; i++) {
    const n = u16(avcC, off)
    off += 2
    if (off + n > avcC.byteLength) {
      pps.push(`truncated(${n})`)
      break
    }
    pps.push(`${n}B nal=${avcC[off] & 0x1f}`)
    off += n
  }
  return (
    `ver=${ver} profile=0x${profile.toString(16)} compat=0x${compat.toString(16)} ` +
    `level=${level} naluLen=${naluLen} sps=${spsN}[${sps}] pps=${ppsN}[${pps}]`
  )
}

function parseNalus(payload, naluLen) {
  const sizeBytes = naluLen > 0 ? naluLen : 4
  const nals = []
  let off = 0
  let ok = true
  while (off < payload.byteLength) {
    if (off + sizeBytes > payload.byteLength) {
      nals.push(`truncated-len@${off}`)
      ok = false
      break
    }
    let n = 0
    for (let i = 0; i < sizeBytes; i++) n = (n << 8) | payload[off + i]
    off += sizeBytes
    if (n < 0 || off + n > payload.byteLength) {
      nals.push(`bad-size=${n} remain=${payload.byteLength - off}`)
      ok = false
      break
    }
    const hdr = payload[off]
    const t = hdr & 0x1f
    nals.push(`${nalName(t)} ${n}B`)
    off += n
  }
  return { ok, nals, left: payload.byteLength - off }
}

function parseAsc(asc) {
  if (asc.byteLength < 2) return `asc too short ${asc.byteLength}B hex=${hex_preview(asc)}`
  const w = (asc[0] << 8) | asc[1]
  let obj = (w >> 11) & 0x1f
  let freqIdx = (w >> 7) & 0x0f
  let ch = (w >> 3) & 0x0f
  if (obj === 31) return `asc escape hex=${hex_preview(asc)}`
  const hz = freqIdx < AAC_FREQ.length ? `${AAC_FREQ[freqIdx]}Hz` : `freqIdx=${freqIdx}`
  return `aac-lc obj=${obj} ${hz} ch=${ch} hex=${hex_preview(asc)}`
}

export class FlvDemuxer {
  constructor(mod, onMedia) {
    this.mod = mod || 'flv-demux'
    this.onMedia = typeof onMedia === 'function' ? onMedia : null
    this.reset()
  }

  reset() {
    this.buf = new Uint8Array(0)
    this.headerDone = false
    this.tags = 0
    this.audioTags = 0
    this.videoTags = 0
    this.scriptTags = 0
    this.bytes = 0
    this.avcSeq = false
    this.aacSeq = false
    this.naluLen = 4
    this.lastAudioTs = -1
    this.lastVideoTs = -1
    this.lastAnyTs = -1
    this.warnedBadMagic = false
  }

  push(chunk) {
    if (!chunk || chunk.byteLength === 0) return
    this.bytes += chunk.byteLength
    this.buf = concatU8(this.buf, copyU8(chunk))
    this.consumeHeader()
    if (this.headerDone) this.consumeTags()
  }

  consumeHeader() {
    if (this.headerDone) return
    if (this.buf.byteLength < 13) return

    const magic = String.fromCharCode(this.buf[0], this.buf[1], this.buf[2])
    const ver = this.buf[3]
    const flags = this.buf[4]
    const offset = u32(this.buf, 5)
    const prev0 = u32(this.buf, 9)

    if (magic !== 'FLV') {
      if (!this.warnedBadMagic) {
        this.warnedBadMagic = true
        log_warn(
          this.mod,
          `首包不是 FLV header magic=${JSON.stringify(magic)} hex=${hex_preview(this.buf, 16)}`,
        )
      }
      if (this.buf.byteLength > 64 * 1024) {
        this.buf = copyU8(this.buf.subarray(this.buf.byteLength - 4096))
      }
      return
    }

    if (ver !== 1) log_warn(this.mod, `header version=${ver}（期望 1）`)
    if (offset !== 9) log_warn(this.mod, `header dataOffset=${offset}（期望 9）`)
    if (prev0 !== 0) log_warn(this.mod, `prevTagSize0=${prev0}（期望 0）`)
    if ((flags & 0x01) === 0) log_warn(this.mod, 'header 未标 hasVideo')
    if ((flags & 0x04) === 0) log_warn(this.mod, 'header 未标 hasAudio')

    log_info(
      this.mod,
      `FLV header ver=${ver} flags=0x${flags.toString(16)} ` +
        `hasVideo=${(flags & 0x01) !== 0} hasAudio=${(flags & 0x04) !== 0} ` +
        `offset=${offset} prev0=${prev0} hex=${hex_preview(this.buf, 13)}`,
    )
    this.buf = copyU8(this.buf.subarray(13))
    this.headerDone = true
  }

  consumeTags() {
    while (this.buf.byteLength >= 11) {
      const type = this.buf[0]
      const size = u24(this.buf, 1)
      const need = 11 + size + 4
      if (size > 8 * 1024 * 1024) {
        log_warn(
          this.mod,
          `tag 声明 size=${size} 异常，丢弃 1 字节同步 hex=${hex_preview(this.buf, 16)}`,
        )
        this.buf = copyU8(this.buf.subarray(1))
        continue
      }
      if (this.buf.byteLength < need) break

      const ts = tagTs(this.buf, 4)
      const streamId = u24(this.buf, 8)
      const body = this.buf.subarray(11, 11 + size)
      const prev = u32(this.buf, 11 + size)
      this.buf = copyU8(this.buf.subarray(need))

      this.tags += 1
      if (streamId !== 0) {
        log_warn(this.mod, `tag#${this.tags} streamId=${streamId}（期望 0）`)
      }
      if (prev !== 11 + size) {
        log_warn(
          this.mod,
          `tag#${this.tags} prevSize=${prev} 对不上 11+${size}=${11 + size}`,
        )
      }
      this.checkTs(type, ts)
      this.logTag(type, ts, size, body)
    }
  }

  checkTs(type, ts) {
    if (this.lastAnyTs >= 0 && ts + 1000 < this.lastAnyTs) {
      log_warn(this.mod, `tag#${this.tags} ts=${ts} 相对上一包 ${this.lastAnyTs} 回退超过 1s`)
    }
    if (type === 8 && this.lastAudioTs >= 0) {
      const d = ts - this.lastAudioTs
      if (d < 0) log_warn(this.mod, `audio ts 回退 ${this.lastAudioTs} -> ${ts}`)
      else if (d > 200) log_warn(this.mod, `audio ts 间隙 ${d}ms ${this.lastAudioTs} -> ${ts}`)
      this.lastAudioTs = ts
    } else if (type === 8) {
      this.lastAudioTs = ts
    }
    if (type === 9 && this.lastVideoTs >= 0) {
      const d = ts - this.lastVideoTs
      if (d < 0) log_warn(this.mod, `video ts 回退 ${this.lastVideoTs} -> ${ts}`)
      else if (d > 200) log_warn(this.mod, `video ts 间隙 ${d}ms ${this.lastVideoTs} -> ${ts}`)
      this.lastVideoTs = ts
    } else if (type === 9) {
      this.lastVideoTs = ts
    }
    this.lastAnyTs = ts
  }

  logTag(type, ts, size, body) {
    const name = TAG_NAME[type] || `0x${type.toString(16)}`
    const extra = type === 9
      ? this.describeVideo(body)
      : type === 8
        ? this.describeAudio(body)
        : `hex=${hex_preview(body, 24)}`
    if (type === 9) this.videoTags += 1
    else if (type === 8) this.audioTags += 1
    else if (type === 18) this.scriptTags += 1
    const line = `tag#${this.tags} ${name} ts=${ts} size=${size}${extra ? ` ${extra}` : ''}`
    const seq = (type === 9 && body.byteLength > 1 && body[1] === 0)
      || (type === 8 && body.byteLength > 1 && body[1] === 0)
    if (seq || this.tags <= 4) log_info(this.mod, line)
    else log_debug(this.mod, line)
    if (this.onMedia && body.byteLength > 1 && !seq) {
      if (type === 9) {
        this.onMedia('video', size, ts, (body[0] & 0xf0) === 0x10)
      } else if (type === 8) {
        this.onMedia('audio', size, ts, false)
      }
    }
  }

  describeVideo(body) {
    if (body.byteLength < 1) return 'empty'
    const frame = (body[0] >> 4) & 0x0f
    const codec = body[0] & 0x0f
    const key = frame === 1 ? 'key' : frame === 2 ? 'inter' : `frame=${frame}`
    if (codec !== 7) return `${key} codec=${codec}（非 AVC） hex=${hex_preview(body, 16)}`
    if (body.byteLength < 5) return `${key} AVC body<5 hex=${hex_preview(body)}`

    const pkt = body[1]
    const cts = (body[2] << 16) | (body[3] << 8) | body[4]
    const ctsSigned = cts & 0x800000 ? cts - 0x1000000 : cts
    const payload = body.subarray(5)
    const pktName = AVC_PKT[pkt] || `pt=${pkt}`

    if (pkt === 0) {
      this.avcSeq = true
      if (payload.byteLength >= 5) this.naluLen = (payload[4] & 3) + 1
      return `${key}/seq codec=AVC cts=${ctsSigned} avcC=${payload.byteLength}B ${parseAvcC(payload)}`
    }
    if (pkt === 1) {
      if (!this.avcSeq) log_warn(this.mod, `tag#${this.tags} 先到 AVC nalu、还没有 seq header`)
      const parsed = parseNalus(payload, this.naluLen)
      if (!parsed.ok) {
        log_warn(this.mod, `tag#${this.tags} AVC nalu 长度字段异常 ${parsed.nals.join(', ')}`)
      }
      const hasIdr = parsed.nals.some((s) => s.startsWith('IDR'))
      if (key === 'key' && !hasIdr) {
        log_warn(this.mod, `tag#${this.tags} 标了 key 但 NALU 里没有 IDR: ${parsed.nals.join(',')}`)
      }
      return (
        `${key}/nalu codec=AVC cts=${ctsSigned} payload=${payload.byteLength}B ` +
        `nalu=[${parsed.nals.join(', ')}]`
      )
    }
    return `${key}/${pktName} codec=AVC cts=${ctsSigned} payload=${payload.byteLength}B`
  }

  describeAudio(body) {
    if (body.byteLength < 1) return 'empty'
    const fmt = (body[0] >> 4) & 0x0f
    const rate = (body[0] >> 2) & 0x03
    const bits = (body[0] >> 1) & 0x01
    const ch = body[0] & 0x01
    if (fmt !== 10) {
      return `fmt=${fmt} rate=${rate} bits=${bits ? 16 : 8} ch=${ch ? 'stereo' : 'mono'}（非 AAC）`
    }
    if (body.byteLength < 2) return 'AAC body<2'
    const pkt = body[1]
    const payload = body.subarray(2)
    if (pkt === 0) {
      this.aacSeq = true
      return `seq fmt=AAC ${parseAsc(payload)}`
    }
    if (pkt === 1) {
      if (!this.aacSeq) log_warn(this.mod, `tag#${this.tags} 先到 AAC raw、还没有 seq header`)
      return `raw fmt=AAC size=${payload.byteLength}`
    }
    return `${AAC_PKT[pkt] || `pt=${pkt}`} fmt=AAC size=${payload.byteLength}`
  }
}
