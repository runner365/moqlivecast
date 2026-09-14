/** After first media, disconnect if nothing arrives for this long.
 *  比 QUIC idle timeout（20s）更长，避免底层还活着就被上层掐掉。 */
export const MEDIA_STALL_MS = 30000

export class StallWatch {
  private timer = 0
  private lastMs = 0
  private onStall: (() => void) | null = null

  start(onStall: () => void) {
    this.stop()
    this.onStall = onStall
    this.lastMs = performance.now()
    this.timer = window.setInterval(() => this.tick(), 1000)
  }

  touch() {
    if (!this.onStall) return
    this.lastMs = performance.now()
  }

  stop() {
    if (this.timer) {
      window.clearInterval(this.timer)
      this.timer = 0
    }
    this.onStall = null
  }

  private tick() {
    if (!this.onStall) return
    if (performance.now() - this.lastMs < MEDIA_STALL_MS) return
    const cb = this.onStall
    this.stop()
    cb()
  }
}
