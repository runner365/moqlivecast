export type MediaStatsSnapshot = {
  videoFps: number | null
  videoKbps: number | null
  audioKbps: number | null
  gopSec: number | null
}

export const EMPTY_STATS: MediaStatsSnapshot = {
  videoFps: null,
  videoKbps: null,
  audioKbps: null,
  gopSec: null,
}

const WINDOW_MS = 5000

export class MediaStats {
  private videoFrames = 0
  private videoBytes = 0
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
      videoKbps: (this.videoBytes * 8) / 1000 / sec,
      audioKbps: (this.audioBytes * 8) / 1000 / sec,
      gopSec: gop,
    }
    this.videoFrames = 0
    this.videoBytes = 0
    this.audioBytes = 0
    this.windowStart = now
    this.onSnap?.(snap)
  }
}
