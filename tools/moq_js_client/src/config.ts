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

export function defaultPushUrl(path = defaultMediaPath()): string {
  return `${DEFAULT_ORIGIN}${normalizePath(path)}?method=push&&app=live&&stream=123456`
}

export function defaultPullUrl(path = defaultMediaPath()): string {
  return `${DEFAULT_ORIGIN}${normalizePath(path)}?method=pull&&app=live&&stream=123456`
}

export function defaultMoqPushUrl(): string {
  return `${DEFAULT_ORIGIN}/moq?app=live&&stream=123456`
}

export function defaultMoqPullUrl(): string {
  return `${DEFAULT_ORIGIN}/moq?app=live&&stream=123456`
}

/** MoQ pull 播放模式默认值。WebCodecs 为默认路径（延迟更低、不依赖 flv.js/MSE）。
 *  浏览器不支持时会自动回退到 flv.js（见 MoqPullView / playbackMode.ts）。 */
export function defaultMoqPlaybackMode(): 'flvjs' | 'webcodecs' {
  return 'webcodecs'
}

export function defaultHttpFlvPullUrl(): string {
  return 'https://www.webrtcserver.com.cn:4433/live/123456.flv'
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
