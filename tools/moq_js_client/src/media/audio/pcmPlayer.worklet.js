/**
 * PCM 播放 AudioWorkletProcessor。
 *
 * 重要：本文件经 `?url` 原样加载到音频线程，**不经过 Vite 转译** —— 所以必须
 *  - 纯 JS（不能是 .ts）
 *  - 完全自包含（零 import；内部 import 不会被重写）
 *
 * 主线程 postMessage 过来的是交错 Float32 PCM（L,R,L,R...）。
 * 内部维护一个环形缓冲，process() 每个 quantum 取 128 帧；不足则补静音并累计欠载。
 * 主线程用回传的真实消费帧数推算媒体时钟（比积分 AudioContext.currentTime 更准）。
 */

const REPORT_INTERVAL = 10 /* 每 ~10 个 quantum 回传一次 telemetry（128*10/sr ≈ 27ms） */

class PcmRing {
  constructor(capacityFrames, channels) {
    this.cap = capacityFrames
    this.ch = channels
    this.buf = new Float32Array(capacityFrames * channels)
    /* 注意变量名：读指针不能叫 `read` —— 会覆盖原型上的 read() 方法 */
    this.readPos = 0
    this.count = 0 /* 已填充帧数 */
    this.droppedOldest = 0 /* 因溢出丢弃的最旧帧数（时钟补偿用） */
  }

  /**
   * 写入并返回写入帧数。空间不足时**丢弃最旧**的数据腾地方（保实时）——
   * 丢最新会让播放位置越拖越远，直播场景必须丢旧的追赶 live。
   * 被丢弃的帧数由调用方计入时钟补偿。
   */
  write(interleaved, frames) {
    const ch = this.ch
    /* 单块就超过整个环：只保留最后 cap 帧 */
    if (frames >= this.cap) {
      const skip = frames - this.cap
      for (let i = 0; i < this.cap; i++) {
        const src = (skip + i) * ch
        const dst = i * ch
        for (let c = 0; c < ch; c++) this.buf[dst + c] = interleaved[src + c]
      }
      this.readPos = 0
      this.count = this.cap
      this.droppedOldest += skip
      return this.cap
    }
    /* 腾出空间：从最旧开始丢 */
    const overflow = this.count + frames - this.cap
    if (overflow > 0) {
      this.readPos = (this.readPos + overflow) % this.cap
      this.count -= overflow
      this.droppedOldest += overflow
    }
    for (let i = 0; i < frames; i++) {
      const dst = ((this.readPos + this.count + i) % this.cap) * ch
      const src = i * ch
      for (let c = 0; c < ch; c++) this.buf[dst + c] = interleaved[src + c]
    }
    this.count += frames
    return frames
  }

  /** 取 frames 帧到 out（交错），不足的补 0；返回真实取到的帧数 */
  read(out, frames) {
    const n = frames < this.count ? frames : this.count
    const ch = this.ch
    for (let i = 0; i < n; i++) {
      const src = ((this.readPos + i) % this.cap) * ch
      const dst = i * ch
      for (let c = 0; c < ch; c++) out[dst + c] = this.buf[src + c]
    }
    if (n < frames) {
      out.fill(0, n * ch)
    }
    this.readPos = (this.readPos + n) % this.cap
    this.count -= n
    return n
  }

  clear() {
    this.readPos = 0
    this.count = 0
    this.droppedOldest = 0
  }
}

class PcmPlayerProcessor extends AudioWorkletProcessor {
  constructor(options) {
    super()
    const opts = (options && options.processorOptions) || {}
    this.channels = opts.channels || 2
    /* 环形缓冲时长：直播延迟 vs 抗欠载的权衡。
     * 500ms → 300ms：降约 200ms 延迟；RTT 低的链路够稳，欠载主要看网络抖动。 */
    this.ring = new PcmRing(Math.round(sampleRate * 0.3), this.channels)
    this.quantumCount = 0
    this.consumedFrames = 0 /* 累计真实消费帧数（不含补的静音） */
    this.underrunFrames = 0 /* 累计欠载补的静音帧数 */
    this.droppedFrames = 0 /* 因缓冲满被丢弃的帧数 */
    this.port.onmessage = (e) => this.onMessage(e.data)
  }

  onMessage(msg) {
    if (!msg) return
    if (msg.type === 'push') {
      const src = msg.interleaved
      const frames = src.length / this.channels
      /* 溢出时 write 内部丢最旧，帧数已记入 ring.droppedOldest */
      this.ring.write(src, frames)
      this.droppedFrames = this.ring.droppedOldest
    } else if (msg.type === 'flush') {
      this.ring.clear()
      this.consumedFrames = 0
      this.underrunFrames = 0
      this.droppedFrames = 0
      this.port.postMessage({ type: 'flushed' })
    }
  }

  process(_inputs, outputs) {
    const out = outputs[0]
    if (!out || out.length === 0) return true
    const quantum = out[0].length /* 通常 128 */

    /* 交错读出来再拆到各声道 */
    const inter = new Float32Array(quantum * this.channels)
    const got = this.ring.read(inter, quantum)
    this.consumedFrames += got
    this.underrunFrames += quantum - got

    for (let c = 0; c < out.length; c++) {
      const ch = out[c]
      for (let i = 0; i < quantum; i++) {
        ch[i] = c < this.channels ? inter[i * this.channels + c] : 0
      }
    }

    this.quantumCount++
    if (this.quantumCount % REPORT_INTERVAL === 0) {
      this.port.postMessage({
        type: 'telemetry',
        consumedFrames: this.consumedFrames,
        underrunFrames: this.underrunFrames,
        /* 注意：droppedOldest 记在 ring 上，不是 processor 上 */
        droppedFrames: this.ring.droppedOldest,
        droppedOldest: this.ring.droppedOldest,
        ringFrames: this.ring.count,
      })
    }
    return true /* 保持存活 */
  }
}

registerProcessor('pcm-player', PcmPlayerProcessor)
