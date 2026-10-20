export type MediaStatsSnapshot = {
  videoFps: number | null
  /** 收到的音频包速率（AAC 访问单元/秒）。48kHz、1024 样本/帧时标称 ≈46.9/s，
   *  44.1kHz 时 ≈43.1/s —— 明显低于标称就是有音频包没到（丢包/发布端跳帧）。
   *  注意它统计的是**到达**的包，播放端环形缓冲内部的丢弃不体现在这里。 */
  audioFps: number | null
  videoKbps: number | null
  audioKbps: number | null
  gopSec: number | null
}

export const EMPTY_STATS: MediaStatsSnapshot = {
  videoFps: null,
  audioFps: null,
  videoKbps: null,
  audioKbps: null,
  gopSec: null,
}

const WINDOW_MS = 5000

export class MediaStats {
  private videoFrames = 0
  private videoBytes = 0
  /** 音频**包**数（addAudio 每调一次算一个 AAC 访问单元）。 */
  private audioFrames = 0
  private audioBytes = 0
  private lastKeyTs = -1
  private prevKeyTs = -1
  private windowStart = 0
  private timer = 0
  private onSnap: ((s: MediaStatsSnapshot) => void) | null = null

  start(onSnap: (s: MediaStatsSnapshot) => void) {
    this.stop()
    this.onSnap = onSnap
    this.windowStart = performance.now()
    this.onSnap(EMPTY_STATS)
    this.timer = window.setInterval(() => this.flush(), WINDOW_MS)
  }

  stop() {
    if (this.timer) {
      window.clearInterval(this.timer)
      this.timer = 0
    }
    this.onSnap = null
    this.videoFrames = 0
    this.videoBytes = 0
    this.audioFrames = 0
    this.audioBytes = 0
    this.lastKeyTs = -1
    this.prevKeyTs = -1
  }

  addVideo(size: number, tsMs: number, key: boolean) {
    this.videoFrames++
    this.videoBytes += size
    if (!key) return
    if (this.lastKeyTs >= 0) this.prevKeyTs = this.lastKeyTs
    this.lastKeyTs = tsMs
  }

  addAudio(size: number) {
    this.audioFrames++
    this.audioBytes += size
  }

  private flush() {
    const now = performance.now()
    const sec = Math.max(0.001, (now - this.windowStart) / 1000)
    const gop = this.prevKeyTs >= 0 && this.lastKeyTs > this.prevKeyTs
      ? (this.lastKeyTs - this.prevKeyTs) / 1000
      : null
    const snap: MediaStatsSnapshot = {
      videoFps: this.videoFrames / sec,
      audioFps: this.audioFrames / sec,
      videoKbps: (this.videoBytes * 8) / 1000 / sec,
      audioKbps: (this.audioBytes * 8) / 1000 / sec,
      gopSec: gop,
    }
    this.videoFrames = 0
    this.videoBytes = 0
    this.audioFrames = 0
    this.audioBytes = 0
    this.windowStart = now
    this.onSnap?.(snap)
  }
}
