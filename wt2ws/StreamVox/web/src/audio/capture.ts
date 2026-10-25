/** Linear resampler that emits fixed 100 ms PCM16 chunks for the WS uplink. */
class Downsampler {
  private buffer: number[] = []

  constructor(
    private readonly inRate: number,
    private readonly outRate = 16000,
    private readonly chunkSamples = 1600,
  ) {}

  push(input: Float32Array, emit: (pcm: Int16Array) => void): void {
    for (let i = 0; i < input.length; i++) this.buffer.push(input[i])
    const ratio = this.inRate / this.outRate
    const need = Math.floor(this.chunkSamples * ratio)
    while (this.buffer.length >= need) {
      emit(this.resample(this.chunkSamples, ratio))
      this.buffer.splice(0, need)
    }
  }

  flush(emit: (pcm: Int16Array) => void): void {
    const ratio = this.inRate / this.outRate
    const count = Math.floor(this.buffer.length / ratio)
    if (count > 0) emit(this.resample(count, ratio))
    this.buffer = []
  }

  private resample(count: number, ratio: number): Int16Array {
    const out = new Int16Array(count)
    for (let i = 0; i < count; i++) {
      const pos = i * ratio
      const i0 = Math.floor(pos)
      const frac = pos - i0
      const a = this.buffer[i0] ?? 0
      const b = this.buffer[i0 + 1] ?? a
      const sample = Math.max(-1, Math.min(1, a + (b - a) * frac))
      out[i] = sample < 0 ? sample * 0x8000 : sample * 0x7fff
    }
    return out
  }
}

export class MicCapture {
  private stream: MediaStream | null = null
  private ctx: AudioContext | null = null
  private source: MediaStreamAudioSourceNode | null = null
  private node: AudioWorkletNode | null = null
  private mute: GainNode | null = null
  private downsampler: Downsampler | null = null
  private emit: (pcm: Int16Array) => void = () => {}
  private running = false

  async start(onChunk: (pcm: Int16Array) => void): Promise<void> {
    this.emit = onChunk
    this.stream = await navigator.mediaDevices.getUserMedia({
      audio: {
        echoCancellation: true,
        noiseSuppression: true,
        autoGainControl: true,
        channelCount: 1,
        sampleRate: 16000,
      },
    })

    // Ask for 16 kHz; if the browser refuses, capture at its rate and resample.
    try {
      this.ctx = new AudioContext({ sampleRate: 16000 })
    } catch {
      this.ctx = new AudioContext()
    }
    await this.ctx.audioWorklet.addModule('/pcm-worklet.js')

    this.source = this.ctx.createMediaStreamSource(this.stream)
    this.node = new AudioWorkletNode(this.ctx, 'pcm-capture')
    this.mute = this.ctx.createGain()
    this.mute.gain.value = 0

    this.downsampler = new Downsampler(this.ctx.sampleRate, 16000)
    this.node.port.onmessage = (event: MessageEvent<ArrayBuffer>) => {
      const samples = new Float32Array(event.data)
      this.downsampler?.push(samples, this.emit)
    }

    this.source.connect(this.node)
    this.node.connect(this.mute)
    this.mute.connect(this.ctx.destination)
    this.running = true
  }

  async stop(): Promise<void> {
    if (!this.running) return
    this.running = false
    if (this.downsampler) this.downsampler.flush(this.emit)
    this.node?.port.close()
    this.source?.disconnect()
    this.node?.disconnect()
    this.mute?.disconnect()
    this.stream?.getTracks().forEach((track) => track.stop())
    if (this.ctx && this.ctx.state !== 'closed') await this.ctx.close()
    this.stream = null
    this.ctx = null
    this.source = null
    this.node = null
    this.mute = null
    this.downsampler = null
  }
}
