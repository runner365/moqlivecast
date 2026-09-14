import workletUrl from './audio/pcmPlayer.worklet.js?url'
import { log_error, log_info, log_warn } from '../logger'

/**
 * PCM 播放器：AudioContext + AudioWorkletNode + 环形缓冲。
 *
 * 主时钟语义：worklet 回传 `consumedFrames`（真实消费的帧数，不含欠载补的静音）。
 * 媒体时钟 = basePtsUs + (consumedFrames - underrunFrames) / sampleRate * 1e6
 * —— 欠载期间时钟自动停走，避免视频被滞后的音频时钟冻住。
 */
export type AudioTelemetry = {
  consumedFrames: number
  underrunFrames: number
  /** 因缓冲溢出丢弃的最旧帧数（直播追帧导致） */
  droppedOldest: number
  droppedFrames: number
  ringFrames: number
}

export class AudioPlayer {
  private ctx: AudioContext | null = null
  private node: AudioWorkletNode | null = null
  private gain: GainNode | null = null

  private sampleRateHz = 48000
  private channels = 2
  private closed = false

  /** 写入 worklet 的第一块的媒体时间戳（微秒），媒体时钟的原点 */
  private basePtsUs = -1
  private lastTelemetry: AudioTelemetry = {
    consumedFrames: 0,
    underrunFrames: 0,
    droppedOldest: 0,
    droppedFrames: 0,
    ringFrames: 0,
  }
  private ready = false

  /** 水位上报回调（主线程据此做统计/告警） */
  onTelemetry: ((t: AudioTelemetry) => void) | null = null

  /** 兜底：autoplay 策略可能把 context 挂起，用户第一次点页面时恢复。
   *  浏览器要求 resume() 必须在用户手势里调用。 */
  private readonly onUserGesture = () => {
    const ctx = this.ctx
    if (!ctx) {
      document.removeEventListener('pointerdown', this.onUserGesture)
      return
    }
    if (ctx.state === 'suspended') {
      void ctx.resume().then(
        () => log_info('wc-audio', `resumed by user gesture, state=${ctx.state}`),
        (e) => log_error('wc-audio', `resume failed: ${errMsg(e)}`),
      )
    }
    if (ctx.state === 'running') {
      document.removeEventListener('pointerdown', this.onUserGesture)
    }
  }

  /**
   * 同步创建 AudioContext 并 resume。
   * 必须在用户手势的同一任务里调用（await 之后再建会被 autoplay 策略挂起）。
   */
  warmup(sampleRate: number, channels: number) {
    this.sampleRateHz = sampleRate > 0 ? sampleRate : 48000
    this.channels = channels > 0 ? channels : 2
    try {
      this.ctx = new AudioContext({ sampleRate: this.sampleRateHz })
      /* 手势任务里立刻 resume；失败也不致命（onUserGesture 会兜底重试） */
      void this.ctx.resume().catch(() => {})
      document.addEventListener('pointerdown', this.onUserGesture)
      log_info('wc-audio', `warmup ctx sampleRate=${this.ctx.sampleRate} state=${this.ctx.state}`)
    } catch (e) {
      log_error('wc-audio', `AudioContext create failed: ${errMsg(e)}`)
      this.ctx = null
    }
  }

  /** 异步加载 worklet 模块并接线。warmup() 之后调用。 */
  async init(): Promise<boolean> {
    const ctx = this.ctx
    if (!ctx) {
      log_error('wc-audio', 'init failed: no AudioContext')
      return false
    }
    try {
      /* autoplay 策略可能把 warmup 时创建的 context 挂起；这里再试一次 */
      if (ctx.state === 'suspended') {
        await ctx.resume()
        log_info('wc-audio', `resumed, state=${ctx.state}`)
      }
      await ctx.audioWorklet.addModule(workletUrl)
      const node = new AudioWorkletNode(ctx, 'pcm-player', {
        numberOfInputs: 0,
        numberOfOutputs: 1,
        outputChannelCount: [this.channels],
        processorOptions: { channels: this.channels },
      })
      node.port.onmessage = (e) => this.onWorkletMsg(e.data)
      const gain = ctx.createGain()
      gain.gain.value = 1
      node.connect(gain)
      gain.connect(ctx.destination)
      this.node = node
      this.gain = gain
      this.ready = true
      log_info('wc-audio', `worklet ready ch=${this.channels} sr=${ctx.sampleRate}`)
      return true
    } catch (e) {
      log_error('wc-audio', `addModule/connect failed: ${errMsg(e)}`)
      return false
    }
  }

  private pushedFrames = 0
  private lastTelemLogMs = 0

  private onWorkletMsg(msg: unknown) {
    if (!msg || typeof msg !== 'object') return
    const m = msg as { type?: string } & Partial<AudioTelemetry>
    if (m.type === 'telemetry') {
      this.lastTelemetry = {
        consumedFrames: m.consumedFrames ?? 0,
        underrunFrames: m.underrunFrames ?? 0,
        droppedOldest: m.droppedOldest ?? 0,
        droppedFrames: m.droppedFrames ?? 0,
        ringFrames: m.ringFrames ?? 0,
      }
      this.onTelemetry?.(this.lastTelemetry)
      /* 每秒打一次水位：ringFrames>0 说明 worklet 没在消费（context 挂起/未接线） */
      const now = performance.now()
      if (now - this.lastTelemLogMs > 1000) {
        this.lastTelemLogMs = now
        log_info(
          'wc-audio',
          `telem pushed=${this.pushedFrames} consumed=${this.lastTelemetry.consumedFrames} ` +
            `under=${this.lastTelemetry.underrunFrames} dropped=${this.lastTelemetry.droppedFrames} ` +
            `ring=${this.lastTelemetry.ringFrames} ctx=${this.ctx?.state ?? 'null'}`,
        )
      }
    }
  }

  get isReady(): boolean {
    return this.ready
  }

  /**
   * 推入一块 PCM（交错 Float32）。ptsUs = 这块的媒体时间戳（微秒）。
   * 第一块会把 basePtsUs 记下来，作为媒体时钟原点。
   */
  push(interleaved: Float32Array, ptsUs: number) {
    const node = this.node
    if (this.closed || !node) return
    if (this.basePtsUs < 0) {
      this.basePtsUs = ptsUs
      log_info('wc-audio', `media clock base set ptsUs=${ptsUs}`)
    }
    /* 每帧 4 字节（f32）；空间不足由 worklet 侧丢弃并计入 droppedFrames */
    this.pushedFrames += interleaved.length / this.channels
    node.port.postMessage({ type: 'push', interleaved, ptsUs })
  }

  /** 清空缓冲并重置时钟（解码器重建 / 断流恢复时用） */
  flush() {
    this.node?.port.postMessage({ type: 'flush' })
    this.basePtsUs = -1
  }

  /**
   * 媒体时钟（微秒）。**只有音频真的开始消费后才有效**，否则返回 -1 让调用方回退。
   *
   * 关键：因溢出丢弃的最旧帧（droppedOldest）也要计入"已越过的时间"——
   * 那些帧的媒体时间已经走过去了，只是没播出来。加上它时钟才能追上 live，
   * 否则视频队列里始终是"未来"的帧、永远渲染不出来（画面冻住）。
   */
  mediaTimeUs(): number {
    if (!this.ready || this.basePtsUs < 0) return -1
    const t = this.lastTelemetry
    /* 正在出声的位置 = 写入序列里的第 (已播 + 已丢) 帧。
     *
     * 不要减去 underrunFrames：那是缓冲空时补的静音帧，**补的静音不占写入序列**，
     * 但它们确实占用了真实时间 —— 之后播到的是更新的数据，位置已经前进。
     * 减掉它会让音频时钟凭空落后（实测 ~0.64s），视频跟着落后 → 音画错位。 */
    const played = t.consumedFrames + t.droppedOldest
    if (played <= 0) return -1 /* 还没真正播放过 → 时钟不可用 */
    const us = this.basePtsUs + (played / this.sampleRateHz) * 1e6
    /* 诊断：拆开看时钟的三个组成部分，确认"正在出声"到底对应哪个媒体时间。
     * 出声位置 = 已写入的最新数据 - 还在缓冲里没播的 = pushed - ring */
    const now = performance.now()
    if (now - this.lastClockLogMs > 2000) {
      this.lastClockLogMs = now
      const sec = (n: number) => (n / this.sampleRateHz).toFixed(2)
      log_info(
        'wc-audio',
        `clock=${(us / 1000).toFixed(0)}ms | pushed=${sec(this.pushedFrames)}s ` +
          `consumed=${sec(t.consumedFrames)}s under=${sec(t.underrunFrames)}s ` +
          `droppedOldest=${sec(t.droppedOldest)}s ring=${sec(t.ringFrames)}s`,
      )
    }
    return us
  }

  private lastClockLogMs = 0

  getTelemetry(): AudioTelemetry {
    return { ...this.lastTelemetry }
  }

  close() {
    if (this.closed) return
    this.closed = true
    document.removeEventListener('pointerdown', this.onUserGesture)
    try {
      this.node?.disconnect()
    } catch {
      /* ignore */
    }
    try {
      this.gain?.disconnect()
    } catch {
      /* ignore */
    }
    try {
      void this.ctx?.close()
    } catch {
      /* ignore */
    }
    this.node = null
    this.gain = null
    this.ctx = null
    this.ready = false
    log_info('wc-audio', 'closed')
  }
}

function errMsg(e: unknown): string {
  return e instanceof Error ? e.message : String(e)
}
