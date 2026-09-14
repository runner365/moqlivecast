/** 播放模式：legacy = flv.js + MSE；webcodecs = VideoDecoder + WebGL / AudioDecoder + AudioWorklet */
export type PlaybackMode = 'flvjs' | 'webcodecs'

/**
 * 探测浏览器是否具备 WebCodecs 播放能力。
 * 四项缺一不可：
 *  - VideoDecoder / AudioDecoder（WebCodecs，Chrome 94+）
 *  - AudioWorklet（音频渲染）
 *  - WebGL2（视频渲染；texImage2D 直接上传 VideoFrame 需要它）
 */
export function isWebCodecsPlaybackSupported(): boolean {
  if (typeof window === 'undefined') return false
  if (typeof (window as unknown as { VideoDecoder?: unknown }).VideoDecoder !== 'function') return false
  if (typeof (window as unknown as { AudioDecoder?: unknown }).AudioDecoder !== 'function') return false
  if (typeof AudioWorkletNode === 'undefined') return false
  if (typeof AudioContext === 'undefined') return false
  try {
    const c = document.createElement('canvas')
    if (!c.getContext('webgl2')) return false
  } catch {
    return false
  }
  return true
}

export function playbackModeLabel(mode: PlaybackMode): string {
  return mode === 'webcodecs' ? 'WebCodecs' : 'flv.js (MSE)'
}
