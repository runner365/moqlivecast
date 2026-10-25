import { base64ToInt16 } from './pcm'

/** Plays the server's PCM16 mono stream back-to-back.
 *
 * The buffer rate is negotiated per session (`session.audio.output.format.rate`)
 * so TTS_SAMPLE_RATE=16000 / 24000 both play at the correct pitch.  The output
 * AudioContext is created at the negotiated rate; if it changes mid-session the
 * browser resamples the buffer.
 */
export class PcmPlayer {
  private ctx: AudioContext | null = null
  private nextTime = 0
  private bufferRate = 16000

  setSampleRate(rate: number): void {
    if (Number.isFinite(rate) && rate >= 8000 && rate <= 48000) {
      this.bufferRate = rate
    }
  }

  async resume(): Promise<void> {
    if (!this.ctx) {
      try {
        this.ctx = new AudioContext({ sampleRate: this.bufferRate })
      } catch {
        this.ctx = new AudioContext()
      }
    }
    if (this.ctx.state === 'suspended') await this.ctx.resume()
  }

  playBase64(b64: string): void {
    if (!this.ctx || !b64) return
    const pcm = base64ToInt16(b64)
    if (!pcm.length) return
    const floats = new Float32Array(pcm.length)
    for (let i = 0; i < pcm.length; i++) floats[i] = pcm[i] / 32768
    const buffer = this.ctx.createBuffer(1, floats.length, this.bufferRate)
    buffer.copyToChannel(floats, 0)

    const source = this.ctx.createBufferSource()
    source.buffer = buffer
    source.connect(this.ctx.destination)

    const now = this.ctx.currentTime
    if (this.nextTime < now + 0.02) this.nextTime = now + 0.05
    source.start(this.nextTime)
    this.nextTime += buffer.duration
  }

  reset(): void {
    this.nextTime = 0
  }

  async close(): Promise<void> {
    if (this.ctx && this.ctx.state !== 'closed') await this.ctx.close()
    this.ctx = null
    this.nextTime = 0
  }
}
