import workletUrl from './audio/pcmPlayer.worklet.js?url'
import { log_error, log_info, log_warn } from '../logger'

/**
 * PCM 播放器：AudioContext + AudioWorkletNode + 环形缓冲。
 *
 * 主时钟语义：worklet 回传 `consumedFrames`（真实消费的帧数，不含欠载补的静音）
 * 与 `droppedOldest`（因环满被丢掉的最旧帧数）。
 * 媒体时钟 = basePtsUs + (consumedFrames + droppedOldest) / sampleRate * 1e6
 * —— "正在出声的位置 = 写入序列里的第 (已播 + 已丢) 帧"。
 * 两个修正项都不要省，理由见 mediaTimeUs() 的注释。
 *
 * ⚠️ 这个公式还有个前提：**写入序列在媒体时间上是连续的**（第 N 个写入帧的 pts
 *    应等于 basePtsUs + N/sr）。真实的流会缺帧（实测服务端丢 3.4% 音频帧，
 *    源文件本身一帧不缺），缺一帧就在 pts 上留一个 ~21ms 空洞。此时"按下标数"
 *    的时钟会比真实 pts 慢，而且**误差逐帧累积**：实测时钟走 0.963x 墙钟、
 *    视频时间轴走 1.000x 墙钟 → 音画差距每秒长 3.7%，两分钟就到 4.6 秒。
 *    视频正是 slave 在这个时钟上（renderTick 的 clockSrc=audio），所以
 *    "差距越来越大"就是这条线。修法：把空洞补成静音，让写入序列重新连续。
 *    ⚠️ 检测必须在**编码块**那一侧做（webcodecsPlayer.pushAudio），再调
 *    padSilenceUs() 注进来 —— 解码后的 AudioData.timestamp 实测是连续的
 *    （本文件的 decGapMax 恒为 0），在 push() 里比 pts 永远不会命中。
 */
/** 环形缓冲时长（秒）。**这是本路径唯一有效的追直播落点** ——
 *  服务端 1x 推、这里 1x 播，积压不会自己消，环满了顶掉最旧，
 *  播放位置就被顶到 (最新 - 这个值)，稳态延迟等于它。
 *  取值 = 抗网络抖动所需 vs 直播延迟的权衡。见 pcmPlayer.worklet.js 的说明。 */
/* ⚠️ 0.5s 太紧了 —— 实测 0.5s 的环在真实链路抖动下累计丢了 231 秒音频
 * （dropped=11082400 帧），听感就是「声音断断续续、像丢包」。
 * 而手机→服务端是**完整的**（服务端 delivered 稳定 47 帧/秒），
 * 服务端→浏览器也稳定（tx rate 平稳、队列零丢弃）—— 丢就丢在这一个环上。
 * 1.5s 是延迟与抗抖动的折中：稳态延迟就是这个值。要更低延迟就往下调，
 * 要更稳就往上调。 */
const AUDIO_RING_SECONDS = 1.5

/** 单次补静音上限（微秒）：超过它说明是时间轴断点（服务端重锚/重启），不是
 *  丢帧 —— 硬补会把时钟推歪，只补到上限并报警（残余误差有界）。
 *  触发阈值在检测侧（webcodecsPlayer 的 AUDIO_GAP_MIN_US），不在这里。 */
const MAX_GAP_PAD_US = 200_000

export type AudioTelemetry = {
  consumedFrames: number
  underrunFrames: number
  /** 因缓冲溢出丢弃的最旧帧数（直播追帧导致） */
  droppedOldest: number
  droppedFrames: number
  ringFrames: number
  /** 环容量（帧），以及它对应的秒数 —— 用于把 droppedOldest 换算成人能读的量 */
  ringCapFrames?: number
  ringSeconds?: number
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
        processorOptions: { channels: this.channels, ringSeconds: AUDIO_RING_SECONDS },
      })
      node.port.onmessage = (e) => this.onWorkletMsg(e.data)
      const gain = ctx.createGain()
      gain.gain.value = 1
      node.connect(gain)
      gain.connect(ctx.destination)
      this.node = node
      this.gain = gain
      this.ready = true
      log_info(
        'wc-audio',
        `worklet ready ch=${this.channels} sr=${ctx.sampleRate} ring=${AUDIO_RING_SECONDS}s ` +
          `(ring length = live catch-up target = steady-state latency)`,
      )
      return true
    } catch (e) {
      log_error('wc-audio', `addModule/connect failed: ${errMsg(e)}`)
      return false
    }
  }

  private pushedFrames = 0
  /** worklet 就绪前被丢掉的帧数（诊断；正常应恒为 0）。 */
  private droppedBeforeReady = 0
  private warnedNoNode = false
  /** 上一块写入后的媒体时间（微秒，块尾 pts）。-1 = 还没推过 / 刚 flush。
   *  这里只用它统计 maxDecodedGapUs，**不据此补静音**（见 push() 的说明）。 */
  private lastPushEndUs = -1
  /** 为 pts 空洞补进去的静音帧数与次数（诊断用） */
  private padFrames = 0
  private padCount = 0
  /** 诊断：**解码块之间**实测的最大 pts 空洞（微秒）。恒为 0 说明解码器输出的
   *  时间戳是连续的、问不出丢帧 —— 补静音只能放在编码侧检测。 */
  private maxDecodedGapUs = 0
  private lastTelemLogMs = 0
  /** 上一次 telemetry 里看到的 droppedOldest（负数 = 刚 flush 过，账本待重同步） */
  private lastDroppedOldest = 0

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
        ringCapFrames: m.ringCapFrames,
        ringSeconds: m.ringSeconds,
      }
      /* ── 环顶掉一批音频 = 媒体时钟被推前 ──
       * 这件事必须显式可见：掉的是**音频内容**（永远听不到了），而且
       * mediaTimeUs() 会把它算成"已播放"，时钟一次前跳若干秒，
       * 从动的视频渲染器得跟着跳。上一轮就是靠 clock= / pushed= / droppedOldest=
       * 三个数相减才反推出"加入时丢了 3.52s"，太费劲 —— 这里直接报警。
       * lastDroppedOldest < 0 = flush 刚归零，账本对不上（可能还有一条 flush 之前
       * 发出的 telemetry 在路上），这一轮只重新对账、不报警。 */
      const droppedDelta =
        this.lastDroppedOldest < 0
          ? 0
          : (this.lastTelemetry.droppedOldest - this.lastDroppedOldest) / this.sampleRateHz
      this.lastDroppedOldest = this.lastTelemetry.droppedOldest
      if (droppedDelta > 0.25) {
        log_warn(
          'wc-audio',
          `ring overflow: dropped ${droppedDelta.toFixed(2)}s of audio in one report ` +
            `(total ${(this.lastTelemetry.droppedOldest / this.sampleRateHz).toFixed(2)}s, ` +
            `ring=${this.lastTelemetry.ringSeconds ?? '?'}s) ` +
            `→ media clock jumped ahead; video must follow`,
        )
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

  /** Worklet 环按几个声道/多少采样率分配 —— 调用方必须把 PCM 换成这个形状。 */
  get sinkChannels(): number {
    return this.channels
  }

  get sinkSampleRate(): number {
    return this.sampleRateHz
  }

  /**
   * 推入一块 PCM（交错 Float32）。ptsUs = 这块的媒体时间戳（微秒）。
   * 第一块会把 basePtsUs 记下来，作为媒体时钟原点。
   *
   * ⚠️ 这里**不**按 pts 补静音：实测解码后的 AudioData.timestamp 是**连续的**
   * （decGapMax 恒为 0），问不出丢帧。丢帧只体现在**编码块**的 pts 上，
   * 所以检测放在 webcodecsPlayer，再由 padSilenceUs() 显式注进来。
   * 这里只顺带记录解码侧的最大空洞，作为"为什么不能在这层做"的证据。
   */
  push(interleaved: Float32Array, ptsUs: number) {
    const node = this.node
    if (this.closed) return
    if (!node) {
      /* worklet 模块是异步加载的，若首包（订阅时的 GOP 回放）比它先到，
       * 这些 PCM 目前会被**静默丢掉**。丢掉的不只是"迟到的包"——那正是给
       * 环形缓冲垫底的那一段，丢了它环就起不来，之后每秒都会欠载。 */
      this.droppedBeforeReady += interleaved.length / this.channels
      if (!this.warnedNoNode) {
        this.warnedNoNode = true
        log_warn(
          'wc-audio',
          `push before worklet ready — PCM is being dropped (${this.droppedBeforeReady} frames so far); ` +
            `the ring will start empty and underrun`,
        )
      }
      return
    }
    if (this.basePtsUs < 0) {
      this.basePtsUs = ptsUs
      log_info('wc-audio', `media clock base set ptsUs=${ptsUs}`)
    }
    if (this.lastPushEndUs >= 0) {
      const g = ptsUs - this.lastPushEndUs
      if (g > this.maxDecodedGapUs) this.maxDecodedGapUs = g
    }
    /* 每帧 4 字节（f32）；空间不足由 worklet 侧丢弃并计入 droppedFrames */
    const frames = interleaved.length / this.channels
    this.pushedFrames += frames
    this.lastPushEndUs = ptsUs + (frames / this.sampleRateHz) * 1e6
    node.port.postMessage({ type: 'push', interleaved, ptsUs })
  }

  /**
   * 往环里注入一段静音（微秒），把媒体时间轴上的空洞填平。
   *
   * 为什么必须填：解码出的 PCM 是按**内容**顺序背靠背写进环的，而媒体时间轴
   * 上缺了帧就有一段没有内容的真实时间。不填的话内容比时间轴跑得快，而
   * mediaTimeUs() 是"数第几个写入帧"，于是时钟比真实媒体时间慢，且误差逐帧
   * 累积（实测每秒 4.2%，20 秒就 0.9 秒）。视频又 slave 在这个时钟上
   * （clockSrc=audio），所以音画差距单调增大 —— 就是"到后面差距越来越大"。
   * 填完之后"按下标数"重新等于真实媒体时间；顺带把环的消耗速率拉回真实时间
   * （缺帧时写入 < 消费，环会被抽干：实测环常年只剩 0.01~0.04s/0.5s、
   * 每秒欠载 4~5%），填静音后供需相等，环才能维持住水位。
   *
   * 为什么不把时钟改成查 pts 表：那要维护"帧下标 → pts"映射并处理环丢最旧的
   * 偏移，而"时间轴上有内容但我没数据 → 输出静音"本来就是播放器的正确处理
   * （快放才是错的）。
   *
   * 调用方 = webcodecsPlayer：只有**编码块**的 pts 是稀疏的（解码后的
   * AudioData.timestamp 实测连续），所以检测必须在那一侧做。
   */
  padSilenceUs(us: number) {
    const node = this.node
    if (this.closed || !node || us <= 0) return
    let padUs = us
    let clamped = false
    if (padUs > MAX_GAP_PAD_US) {
      padUs = MAX_GAP_PAD_US
      clamped = true
    }
    const frames = Math.round((padUs / 1e6) * this.sampleRateHz)
    if (frames <= 0) return
    const atUs = this.lastPushEndUs >= 0 ? this.lastPushEndUs : 0
    node.port.postMessage({
      type: 'push',
      interleaved: new Float32Array(frames * this.channels),
      ptsUs: atUs,
    })
    this.pushedFrames += frames
    this.padFrames += frames
    this.padCount++
    if (this.lastPushEndUs >= 0) this.lastPushEndUs += padUs
    if (clamped || this.padCount <= 3 || this.padCount % 200 === 0) {
      log_warn(
        'wc-audio',
        `audio gap ${(us / 1000).toFixed(1)}ms -> padded ${(padUs / 1000).toFixed(1)}ms silence ` +
          `(#${this.padCount}, total ${(this.padFrames / this.sampleRateHz).toFixed(2)}s` +
          `${clamped ? ', CLAMPED: timeline discontinuity, clock will lag' : ''})`,
      )
    }
  }

  /** 清空缓冲并重置时钟（解码器重建 / 断流恢复时用） */
  flush() {
    this.node?.port.postMessage({ type: 'flush' })
    this.basePtsUs = -1
    /* 解码器重建后 pts 会换一条时间轴，连续性锚点必须清掉 ——
     * 否则下一块会被当成一个巨大的 pts 空洞，白补几百毫秒静音。 */
    this.lastPushEndUs = -1
    /* worklet 侧 ring.clear() 会把 droppedOldest 归零。置 -1 而不是 0：
     * 可能还有一条 flush 之前发出的 telemetry 在路上，拿它算差值会得出
     * 一个假的"丢了好几秒"告警。负值让下一条只重新对账。 */
    this.lastDroppedOldest = -1
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
          `droppedOldest=${sec(t.droppedOldest)}s ring=${sec(t.ringFrames)}s/${t.ringSeconds ?? '?'}s ` +
          `pad=${sec(this.padFrames)}s/${this.padCount} ` +
          `decGapMax=${(this.maxDecodedGapUs / 1000).toFixed(1)}ms`,
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
