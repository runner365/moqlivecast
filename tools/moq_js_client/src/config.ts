export type MediaPath = {
  desc: string
  path: string
}

/** 与 moqlivecast/etc/config.yml 的 moqlivecast.subpath 对齐 */
export const MEDIA_PATHS: MediaPath[] = [
  { desc: 'media format is flv', path: 'flv' },
  { desc: 'media format is moq', path: 'moq' },
]

export const DEFAULT_ORIGIN = 'https://www.webrtcserver.com.cn:4433'

export function normalizePath(path: string): string {
  if (!path) return '/'
  return path.startsWith('/') ? path : `/${path}`
}

export function defaultMediaPath(): string {
  return MEDIA_PATHS[0]?.path ?? 'flv'
}

/* ── 会话级随机 stream 名 ──
 *
 * 6 位，小写字母 + 数字。用小写是为了避免 stream 名在 URL/路径中的
 * 大小写敏感问题。
 *
 * 关键：push 与 pull 必须拿到【同一个】名字，否则推流写进 live/abc123、
 * 拉流去读 live/xyz789，永远对不上。所以这里用 sessionStorage 做会话级
 * 缓存而非每次调用现生成：
 *   - 同一标签页内切换视图 / 刷新  → 值不变（push 与 pull 能对上）
 *   - 新开标签页                   → 新值（可并行测多路流）
 * sessionStorage 不可用时（隐私模式）退化为每次现生成，此时推拉需手动对齐。 */
const STREAM_KEY = 'moq.stream'
const STREAM_RE = /^[a-z0-9]{6}$/

function randomStream(): string {
  const chars = 'abcdefghijklmnopqrstuvwxyz0123456789'
  const buf = new Uint8Array(6)
  if (typeof crypto !== 'undefined' && crypto.getRandomValues) {
    crypto.getRandomValues(buf)
  } else {
    for (let i = 0; i < 6; i++) buf[i] = Math.floor(Math.random() * 256)
  }
  let s = ''
  for (let i = 0; i < 6; i++) s += chars[buf[i] % chars.length]
  return s
}

/** 当前会话的 stream 名。首次调用时生成并缓存。 */
export function sessionStream(): string {
  try {
    const cached = sessionStorage.getItem(STREAM_KEY)
    if (cached && STREAM_RE.test(cached)) return cached
    const v = randomStream()
    sessionStorage.setItem(STREAM_KEY, v)
    return v
  } catch {
    return randomStream()
  }
}

export function defaultPushUrl(path = defaultMediaPath()): string {
  return `${DEFAULT_ORIGIN}${normalizePath(path)}?method=push&&app=live&&stream=${sessionStream()}`
}

export function defaultPullUrl(path = defaultMediaPath()): string {
  return `${DEFAULT_ORIGIN}${normalizePath(path)}?method=pull&&app=live&&stream=${sessionStream()}`
}

export function defaultMoqPushUrl(): string {
  return `${DEFAULT_ORIGIN}/moq?app=live&&stream=${sessionStream()}`
}

export function defaultMoqPullUrl(): string {
  return `${DEFAULT_ORIGIN}/moq?app=live&&stream=${sessionStream()}`
}

/** MoQ pull 播放模式默认值。WebCodecs 为默认路径（延迟更低、不依赖 flv.js/MSE）。
 *  浏览器不支持时会自动回退到 flv.js（见 MoqPullView / playbackMode.ts）。 */
export function defaultMoqPlaybackMode(): 'flvjs' | 'webcodecs' {
  return 'webcodecs'
}

export function defaultHttpFlvPullUrl(): string {
  /* 与 MoQ 推流同一 stream 名：HTTP-FLV 拉的是同一条流。 */
  return `${DEFAULT_ORIGIN}/live/${sessionStream()}.flv`
}

export function pathFromUrl(url: string): string {
  try {
    return new URL(url).pathname.replace(/^\/+/, '') || defaultMediaPath()
  } catch {
    return defaultMediaPath()
  }
}

export function replaceUrlPath(url: string, path: string): string {
  try {
    const u = new URL(url)
    u.pathname = normalizePath(path)
    return u.toString()
  } catch {
    return url
  }
}
