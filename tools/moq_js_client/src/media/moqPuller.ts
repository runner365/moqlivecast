import flvjs from 'flv.js'
import { sessionStream } from '../config'
import { hex_preview, log_debug, log_error, log_info, log_warn } from '../logger'
import {
  isSubscribeOk,
  parseSubscribeOk,
  tryReadControl,
  tryReadObject,
  tryReadSubgroup,
  type LocObject,
} from '../moq/decode'
import { ByteReader, encodeVarint } from '../moq/varint'
import {
  ALIAS_AUDIO,
  ALIAS_VIDEO,
  encodeSetup,
  encodeSubscribe,
} from '../moq/wire'
import { FlvChunkLoader, FlvChunkSource, bindFlvChunkSource } from './flvChunkLoader'
import { FlvDemuxer } from './flvdemuxer'
import { FlvMuxer } from './flvMuxer'
import { MediaStats, type MediaStatsSnapshot } from './mediaStats'
import type { PlaybackMode } from './playbackMode'
import type { SessionPhase, SessionPhaseHooks } from './sessionPhase'
import { MEDIA_STALL_MS, StallWatch } from './stallWatch'
import { WebCodecsPlayer } from './webcodecsPlayer'

const LIVE_LATENCY_S = 2.0
const MAX_LATENCY_S = 3.5
const SUBSCRIBE_TIMEOUT_MS = 30000

type BufRange = { start: number; end: number }

/** 一个待下发的媒体样本（原来经 jitter buffer 排序，现在直通）。 */
type MediaSample = {
  ts: number
  alias: number
  obj: LocObject
}

/** 单条轨道的归一化状态：lastRaw 用于跳变检测，lastOut 用于条内单调性。
 *  时间轴原点 base 是**音视频共享**的（见 wcOriginMs），不放这里。 */
/* 时间戳归一化状态 + 连续性观测计数（诊断用） */
type TsNorm = {
  lastRaw: number
  lastOut: number
  /* 以下为观测统计，不参与归一化计算 */
  statCount: number      /* 累计帧数 */
  statBack: number       /* rawMs 回退次数 */
  statPinned: number     /* 因回退被强制 +1 拉平的次数 */
  statMinGap: number     /* 最小正间隔 */
  statMaxGap: number     /* 最大间隔 */
}

function newTsNorm(): TsNorm {
  return {
    lastRaw: -1,
    lastOut: -1,
    statCount: 0,
    statBack: 0,
    statPinned: 0,
    statMinGap: Number.MAX_SAFE_INTEGER,
    statMaxGap: 0,
  }
}

function parseAppStream(url: string): { app: string; stream: string } {
  /* URL 缺参数时的兜底用 sessionStream()，而不是写死 '123456'：
   * 保持一致才能让「没填 stream 的推流」与「同标签页的拉流」对上。 */
  try {
    const u = new URL(url)
    return {
      app: u.searchParams.get('app') || 'live',
      stream: u.searchParams.get('stream') || sessionStream(),
    }
  } catch {
    return { app: 'live', stream: sessionStream() }
  }
}

export class MoqPuller {
  private wt: WebTransport | null = null
  private controlReader: ReadableStreamDefaultReader<Uint8Array> | null = null
  private controlWriter: WritableStreamDefaultWriter<Uint8Array> | null = null
  private player: ReturnType<typeof flvjs.createPlayer> | null = null
  private source = new FlvChunkSource()
  private demux = new FlvDemuxer('moq-pull')
  private muxer: FlvMuxer | null = null
  private stats = new MediaStats()
  private closed = false
  private videoEl: HTMLVideoElement | null = null
  private chaseTimer = 0
  private lastSeekMs = 0
  /* config 只在该 track 第一个 object 上出现一次，必须缓存字节：
   * 解码器 reset/重建后要用它重新 configure，光置 bool 恢复不了。 */
  private avcC: Uint8Array | null = null
  private aacAsc: Uint8Array | null = null
  private avcConfigured = false
  private aacConfigured = false
  private tsShift = 0
  private lastFlvTs = -1
  /* 播放路径：legacy = flv.js+MSE；webcodecs = VideoDecoder/WebGL + AudioDecoder/AudioWorklet */
  private mode: PlaybackMode = 'flvjs'
  private wc: WebCodecsPlayer | null = null
  private canvasEl: HTMLCanvasElement | null = null
  /* webcodecs 时间轴归一化（ms）。
   * 原点 wcOriginMs 由音视频**共享**：服务端用同一个 pull_ts_base_ 对齐两条流，
   * 客户端减同一个原点才能保持音画相对偏移；各自归零会把它们强行对齐到 0 → 音画错位。
   * 每条流另有独立的 lastRaw（跳变检测）与 lastOut（条内单调性）。 */
  private wcOriginMs = -1
  private wcNormV: TsNorm = newTsNorm()
  private wcNormA: TsNorm = newTsNorm()
  private wcFirstEmitLogged = false
  private onPhase: ((p: SessionPhase) => void) | null = null
  private mediaSeen = false
  private stall = new StallWatch()
  private connGen = 0

  private readonly onVideoError = () => {
    const err = this.videoEl?.error
    log_error('moq-pull', `video.error code=${err?.code} msg=${err?.message ?? ''}`)
  }
  private readonly onVideoPlaying = () => {
    log_info('moq-pull', `video playing ${this.bufferLog()}`)
  }
  private readonly onVideoWaiting = () => {
    log_warn('moq-pull', `video waiting ${this.bufferLog()}`)
    this.resumeTransmuxer()
    this.chaseLive('waiting')
    void this.videoEl?.play()?.catch(() => {})
  }
  private readonly onVideoStalled = () => {
    log_warn('moq-pull', `video stalled ${this.bufferLog()}`)
    this.resumeTransmuxer()
    this.chaseLive('stalled')
  }
  private readonly onLoadedMeta = () => {
    const v = this.videoEl
    log_info('moq-pull', `video loadedmetadata ${v?.videoWidth}x${v?.videoHeight} ${this.bufferLog()}`)
    this.chaseLive('metadata')
  }

  async start(
    url: string,
    targets: { video?: HTMLVideoElement | null; canvas?: HTMLCanvasElement | null },
    hooks: { onStats?: (s: MediaStatsSnapshot) => void; mode?: PlaybackMode } & SessionPhaseHooks = {},
  ) {
    const mode = hooks.mode ?? 'flvjs'
    const video = targets.video ?? null
    const canvas = targets.canvas ?? null

    /* WebCodecs 需要 AudioContext，而 autoplay 策略要求它在用户手势的同一任务里创建/恢复。
     * 本函数第一个语句就是 await this.stop()，await 之后手势上下文就丢了，
     * 所以必须在此之前同步建出来。注意先不挂到 this.wc —— stop() 会关掉它。 */
    let pendingWc: WebCodecsPlayer | null = null
    if (mode === 'webcodecs') {
      if (!canvas) {
        throw new Error('webcodecs 模式需要 canvas 目标元素')
      }
      log_info('moq-pull', `webcodecs mode: canvas=${canvas.width}x${canvas.height}, warmup`)
      pendingWc = new WebCodecsPlayer({ canvas, stats: this.stats })
      pendingWc.warmup()
    }

    await this.stop()
    this.wc = pendingWc
    const gen = ++this.connGen
    this.closed = false
    this.mode = mode
    this.canvasEl = canvas
    this.avcC = null
    this.aacAsc = null
    this.avcConfigured = false
    this.aacConfigured = false
    this.tsShift = 0
    this.lastFlvTs = -1
    this.wcOriginMs = -1
    this.wcNormV = newTsNorm()
    this.wcNormA = newTsNorm()
    this.wcFirstEmitLogged = false
    this.mediaSeen = false
    this.onPhase = hooks.onPhase ?? null
    this.stats.start((s) => hooks.onStats?.(s))
    if (mode === 'flvjs') {
      this.source = new FlvChunkSource()
      this.demux = new FlvDemuxer('moq-pull', (kind, size, ts, key) => {
        if (kind === 'video') this.stats.addVideo(size, ts, key)
        else this.stats.addAudio(size)
      })
      this.muxer = new FlvMuxer((buf) => {
        this.demux.push(buf)
        this.source.push(buf)
      })
    }
    this.lastSeekMs = 0
    this.videoEl = video
    const id = parseAppStream(url)
    log_info('moq-pull', `connect ${url} app=${id.app} stream=${id.stream}`)

    const wt = new WebTransport(url)
    this.wt = wt
    await wt.ready
    if (gen !== this.connGen || this.wt !== wt) return
    log_info('moq-pull', 'WebTransport ready')
    this.setPhase('connected and waiting for stream')

    const control = await wt.createBidirectionalStream()
    if (gen !== this.connGen || this.wt !== wt) return
    this.controlWriter = control.writable.getWriter()
    this.controlReader = control.readable.getReader()
    log_info('moq-pull', 'control bidi opened')

    await this.writeControl(encodeSetup(), 'SETUP')
    await this.writeControl(
      encodeSubscribe({ requestId: 1, app: id.app, stream: id.stream, trackName: 'video', alias: ALIAS_VIDEO }),
      'SUBSCRIBE video',
    )
    await this.writeControl(
      encodeSubscribe({ requestId: 2, app: id.app, stream: id.stream, trackName: 'audio', alias: ALIAS_AUDIO }),
      'SUBSCRIBE audio',
    )

    await this.waitSubscribeOk(2)
    if (gen !== this.connGen || this.wt !== wt) return
    if (this.mode === 'webcodecs') {
      log_info('moq-pull', 'webcodecs playback mode: decoder sink ready')
    } else {
      if (!video) throw new Error('flvjs 模式需要 video 目标元素')
      this.startPlayer(video)
    }

    void this.readControl()
    void this.openTrack(ALIAS_VIDEO, 'video')
    void this.openTrack(ALIAS_AUDIO, 'audio')

    wt.closed.catch((e) => {
      if (gen !== this.connGen || this.wt !== wt) return
      this.closed = true
      this.setPhase('disconnected')
      log_warn('moq-pull', `WebTransport closed: ${e instanceof Error ? e.message : String(e)}`)
    })
    /* chaseTimer / MSE 追帧逻辑仅 legacy 需要；webcodecs 靠自身渲染循环追帧 */
    if (this.mode === 'flvjs') {
      this.chaseTimer = window.setInterval(() => this.chaseLive('timer'), 500)
    }
  }

  async stop() {
    this.connGen += 1
    this.closed = true
    this.stall.stop()
    if (this.chaseTimer) {
      window.clearInterval(this.chaseTimer)
      this.chaseTimer = 0
    }
    this.unbindVideoEvents()
    this.videoEl = null
    if (this.wc) {
      try { this.wc.close() } catch { /* ignore */ }
      this.wc = null
    }
    this.canvasEl = null
    this.avcC = null
    this.aacAsc = null
    this.avcConfigured = false
    this.aacConfigured = false
    try { this.controlReader?.releaseLock() } catch { /* ignore */ }
    this.controlReader = null
    try { await this.controlWriter?.close() } catch { /* ignore */ }
    this.controlWriter = null
    try { this.player?.pause() } catch { /* ignore */ }
    try { this.player?.unload() } catch { /* ignore */ }
    try { this.player?.detachMediaElement() } catch { /* ignore */ }
    try { this.player?.destroy() } catch { /* ignore */ }
    this.player = null
    bindFlvChunkSource(null)
    try { this.wt?.close() } catch { /* ignore */ }
    this.wt = null
    this.muxer = null
    this.stats.stop()
    this.demux.reset()
    log_info('moq-pull', 'stopped')
  }

  private async writeControl(buf: Uint8Array, label: string) {
    if (!this.controlWriter) throw new Error('control missing')
    await this.controlWriter.ready
    await this.controlWriter.write(buf)
    log_info('moq-pull', `TX ${label} ${buf.byteLength}B hex=${hex_preview(buf, 24)}`)
  }

  private async waitSubscribeOk(need: number) {
    if (!this.controlReader) throw new Error('control reader missing')
    const r = new ByteReader()
    let got = 0
    const t0 = performance.now()
    while (!this.closed && got < need) {
      if (performance.now() - t0 > SUBSCRIBE_TIMEOUT_MS) {
        throw new Error(`SUBSCRIBE_OK timeout (${got}/${need})`)
      }
      const { value, done } = await this.controlReader.read()
      if (done) throw new Error('control stream closed before SUBSCRIBE_OK')
      if (value) r.push(value)
      for (;;) {
        const msg = tryReadControl(r)
        if (!msg) break
        if (!isSubscribeOk(msg)) {
          log_info('moq-pull', `RX control type=0x${msg.type.toString(16)} ${msg.body.byteLength}B`)
          continue
        }
        const ok = parseSubscribeOk(msg.body)
        got += 1
        log_info(
          'moq-pull',
          `RX SUBSCRIBE_OK #${got} req=${ok?.requestId} alias=${ok?.alias} ${msg.body.byteLength}B`,
        )
      }
    }
  }

  private async readControl() {
    if (!this.controlReader) return
    const r = new ByteReader()
    try {
      while (!this.closed && this.controlReader) {
        const { value, done } = await this.controlReader.read()
        if (done) break
        if (value) r.push(value)
        for (;;) {
          const msg = tryReadControl(r)
          if (!msg) break
          log_debug('moq-pull', `RX control type=0x${msg.type.toString(16)} ${msg.body.byteLength}B`)
        }
      }
    } catch (e) {
      if (!this.closed) {
        log_error('moq-pull', `control: ${e instanceof Error ? e.message : String(e)}`)
      }
    }
  }

  private async openTrack(alias: number, name: string) {
    if (!this.wt) return
    log_info('moq-pull', `open bidi ${name} alias=${alias}`)
    const stream = await this.wt.createBidirectionalStream()
    const w = stream.writable.getWriter()
    const bind = encodeVarint(alias)
    await w.ready
    await w.write(bind)
    log_info('moq-pull', `TX bind ${name} alias=${alias} hex=${hex_preview(bind)}`)
    const reader = stream.readable.getReader()
    const r = new ByteReader()
    let header = false
    let hasProps = true
    let objs = 0
    try {
      while (!this.closed) {
        const { value, done } = await reader.read()
        if (done) {
          log_warn('moq-pull', `${name} stream done objs=${objs}`)
          break
        }
        if (value) r.push(value)
        if (!header) {
          const sg = tryReadSubgroup(r)
          if (!sg) continue
          header = true
          hasProps = sg.hasProps
          log_info('moq-pull', `RX SUBGROUP ${name} alias=${sg.alias} props=${hasProps ? 1 : 0}`)
        }
        for (;;) {
          const obj = tryReadObject(r, hasProps)
          if (!obj) break
          objs++
          if (objs <= 3) {
            log_info(
              'moq-pull',
              `object#${objs} ${name} ts=${obj.timestampMs} ${obj.payload.byteLength}B ` +
                `cfg=${obj.videoConfig ? 'V' : ''}${obj.audioConfig ? 'A' : ''} key=${obj.key ? 1 : 0} ` +
                `head=${hex_preview(obj.payload, 12)}`,
            )
          }
          this.onLoc(alias, obj)
        }
      }
    } catch (e) {
      if (!this.closed) {
        log_error('moq-pull', `${name}: ${e instanceof Error ? e.message : String(e)}`)
      }
    } finally {
      try { await w.close() } catch { /* ignore */ }
      try { reader.releaseLock() } catch { /* ignore */ }
    }
  }

  private setPhase(phase: SessionPhase) {
    if (this.closed && phase !== 'disconnected') return
    this.onPhase?.(phase)
  }

  private noteMedia() {
    if (this.closed) return
    if (!this.mediaSeen) {
      this.mediaSeen = true
      this.stall.start(() => this.onMediaStall())
      this.setPhase('receiving stream')
      return
    }
    this.stall.touch()
  }

  private onMediaStall() {
    if (this.closed) return
    log_warn('moq-pull', `no media for ${MEDIA_STALL_MS}ms, disconnect ${this.bufferLog()}`)
    this.setPhase('disconnected')
    void this.stop()
  }

  private mapFlvTs(raw: number): number {
    let out = raw + this.tsShift
    if (this.lastFlvTs >= 0 && out + 1000 < this.lastFlvTs) {
      this.tsShift += this.lastFlvTs + 40 - out
      out = raw + this.tsShift
      log_warn('moq-pull', `ts rewind ${this.lastFlvTs} -> ${raw}, continue at ${out}`)
    }
    if (out < 0) out = 0
    this.lastFlvTs = out
    return out
  }

  /**
   * 媒体直通：不再经 jitter buffer 排队，收到即下发。
   * 排序/起播对齐交给下游（legacy = flv.js transmuxer 的 2s stash；webcodecs = 渲染时钟 +
   * 解码队列），时间戳连续性由 normalizeMediaTs / mapFlvTs 保证。
   */
  private enqueueMedia(alias: number, obj: LocObject) {
    const ts = Math.max(0, obj.timestampMs | 0)
    this.emitSample({ ts, alias, obj })
  }

  /**
   * webcodecs 专用时间轴归一化（单位 ms）。
   *
   * 原点 **音视频共享**（wcOriginMs = 第一个到达样本的 raw ts）：
   * 服务端已用同一个 pull_ts_base_ 把两条流放在同一时间轴上，减同一个原点即可
   * 保留音画相对偏移。各减各的会把它们都压到 0 → 音画错位。
   *
   * 每条流另有独立的单调性保证（n.lastOut）——解码器要求同一条流的时间戳不回退。
   * 首帧允许为负（另一条流先到、本条更早的情况），此时负值正反映真实偏移。
   */
  private normalizeMediaTs(n: TsNorm, rawMs: number, tag: string): number {
    if (this.wcOriginMs < 0) {
      this.wcOriginMs = rawMs
      log_info('moq-pull', `wc shared ts origin = ${rawMs}ms (first sample = ${tag})`)
    }
    let out: number
    if (n.lastRaw < 0) {
      /* 本条流第一帧：直接相对共享原点（可能为负，属正常） */
      out = rawMs - this.wcOriginMs
    } else {
      const dRaw = rawMs - n.lastRaw
      if (dRaw > 5000) {
        /* 向前跳变：服务端重锚了时间轴。原点跟着前移，让输出从上一帧 +40ms 继续。 */
        this.wcOriginMs += dRaw - 40
        log_warn(
          'moq-pull',
          `wc ts forward jump [${tag}] ${n.lastRaw} -> ${rawMs}, origin -> ${this.wcOriginMs}`,
        )
      }
      out = rawMs - this.wcOriginMs
      /* 条内单调：只防回退，不回拉大跳 */
      if (out <= n.lastOut) out = n.lastOut + 1
    }

    /* ── 时间戳连续性观测（诊断用，不改行为）──
     * 输入的 rawMs 是服务端发来的共享时间轴，out 是喂给解码器的值。
     * 关注三件事：
     *   1) rawMs 回退 —— 服务端时间轴倒退，MSE/解码器会缓冲异常
     *   2) out 被强制 +1 —— 说明发生了回退被强行拉平，帧被压在同一时刻
     *   3) out 间隔异常 —— 正常 15fps 应约 66.7ms
     * 每帧打一条 debug，另有每 60 帧一条的汇总。 */
    if (n.lastRaw >= 0) {
      const dRaw = rawMs - n.lastRaw
      if (dRaw < 0) n.statBack++
      if (dRaw > n.statMaxGap) n.statMaxGap = dRaw
      if (dRaw < n.statMinGap) n.statMinGap = dRaw
      if (out === n.lastOut + 1 && dRaw <= 0) n.statPinned++
      log_debug(
        'moq-pull',
        `ts[${tag}] #${n.statCount} raw=${rawMs} out=${out} ` +
          `dRaw=${dRaw} dOut=${n.lastOut >= 0 ? out - n.lastOut : 0} ` +
          `origin=${this.wcOriginMs}${dRaw < 0 ? '  <<< 回退' : ''}`,
      )
    }

    n.statCount++
    n.lastRaw = rawMs
    n.lastOut = out

    if (n.statCount % 60 === 0) {
      log_info(
        'moq-pull',
        `ts统计[${tag}] ${n.statCount}帧 out=${out}ms ` +
          `回退=${n.statBack} 被钉平=${n.statPinned} ` +
          `间隔 min=${n.statMinGap === Number.MAX_SAFE_INTEGER ? '-' : n.statMinGap}` +
          `/max=${n.statMaxGap}ms ` +
          `origin=${this.wcOriginMs}` +
          (n.statBack > 0 ? '  ← 时间戳有回退' : ''),
      )
    }
    return out
  }

  private emitSample(sample: MediaSample) {
    const obj = sample.obj
    if (this.mode === 'webcodecs') {
      const wc = this.wc
      if (!wc) return
      const isVideo = sample.alias === ALIAS_VIDEO
      /* 音视频各用一套归一化状态：两条流的 ts 轴不同、到达交错，
       * 共用会让 base/lastRaw 互相污染，导致帧时间戳远大于时钟、渲染队列堆死。 */
      const norm = isVideo ? this.wcNormV : this.wcNormA
      /* 先按 DTS 归一化，再加 CTS。
       *
       * 顺序不能反：normalizeMediaTs 带单调性保证（out <= lastOut 时强制 +1），
       * 而 PTS 在有 B 帧时本就不单调（显示顺序 ≠ 解码顺序）。
       * 若直接把 pts 喂进去，正常的 B 帧回退会被拉平成一堆同刻帧。
       * DTS 单调，归一化对它是纯平移，所以
       *   normalize(dts) + cts === (dts + cts) - origin === pts - origin
       * 语义等价且不触发单调保护。音频无 B 帧，cts 恒 0。 */
      const dtsOutMs = this.normalizeMediaTs(norm, sample.ts, isVideo ? 'v' : 'a')
      const ctsMs = isVideo ? obj.ctsMs : 0
      const outMs = dtsOutMs + ctsMs
      if (!this.wcFirstEmitLogged) {
        this.wcFirstEmitLogged = true
        log_info(
          'moq-pull',
          `first sample emitted alias=${sample.alias} rawTs=${sample.ts} cts=${ctsMs} ` +
            `dtsOut=${dtsOutMs} ptsOut=${outMs} ` +
            `${obj.payload.byteLength}B key=${obj.key ? 1 : 0}`,
        )
      }
      if (isVideo) {
        /* webcodecs 没有 flv demuxer 旁路，统计必须从这里喂 */
        this.stats.addVideo(obj.payload.byteLength, outMs, obj.key)
        wc.pushVideo(obj.payload, outMs * 1000, obj.key)
        return
      }
      this.stats.addAudio(obj.payload.byteLength)
      wc.pushAudio(obj.payload, outMs * 1000)
      return
    }

    const mux = this.muxer
    if (!mux) return
    const ts = this.mapFlvTs(sample.ts)
    if (sample.alias === ALIAS_VIDEO) {
      /* 第三参是 CTS（写进 FLV tag 的 CompositionTime）。
       * 原先硬编码 0 → pts 恒等于 dts，有 B 帧的流在 MSE 上前后跳。 */
      mux.writeAvcNalu(obj.payload, ts, obj.ctsMs ?? 0, obj.key)
      return
    }
    mux.writeAacRaw(obj.payload, ts)
  }

  private onLoc(alias: number, obj: LocObject) {
    if (this.closed) return
    const wc = this.mode === 'webcodecs' ? this.wc : null
    const mux = this.mode === 'flvjs' ? this.muxer : null
    if (!wc && !mux) {
      log_error(
        'moq-pull',
        `onLoc sink missing: mode=${this.mode} wc=${!!this.wc} mux=${!!this.muxer} alias=${alias}`,
      )
      return
    }
    this.noteMedia()
    if (alias === ALIAS_VIDEO) {
      if (obj.videoConfig) {
        /* config 只发一次；重配置路径也走这里（缓存字节，不是 bool） */
        const first = !this.avcConfigured
        this.avcC = obj.videoConfig.slice()
        if (wc) {
          wc.configureVideo(this.avcC)
          this.avcConfigured = true
        } else if (mux && first) {
          mux.writeAvcSeq(this.avcC, this.mapFlvTs(Math.max(0, obj.timestampMs | 0)))
          this.avcConfigured = true
        }
        log_info(
          'moq-pull',
          `video_config ${this.avcC.byteLength}B hex=${hex_preview(this.avcC, 8)} ${first ? 'first' : 're-config'}`,
        )
      }
      if (obj.payload.byteLength > 0 && !obj.videoConfig) {
        this.enqueueMedia(alias, obj)
      }
      return
    }
    if (alias === ALIAS_AUDIO) {
      if (obj.audioConfig) {
        const first = !this.aacConfigured
        this.aacAsc = obj.audioConfig.slice()
        if (wc) {
          wc.configureAudio(this.aacAsc)
          this.aacConfigured = true
        } else if (mux && first) {
          mux.writeAacSeq(this.aacAsc, this.mapFlvTs(Math.max(0, obj.timestampMs | 0)))
          this.aacConfigured = true
        }
        log_info(
          'moq-pull',
          `audio_config ${this.aacAsc.byteLength}B hex=${hex_preview(this.aacAsc, 4)} ${first ? 'first' : 're-config'}`,
        )
      }
      if (obj.payload.byteLength > 0 && !obj.audioConfig) {
        this.enqueueMedia(alias, obj)
      }
    }
  }

  private startPlayer(video: HTMLVideoElement) {
    if (!flvjs.isSupported()) throw new Error('This browser does not support flv.js / MSE')
    try {
      flvjs.LoggingControl.applyConfig({
        enableDebug: false,
        enableVerbose: false,
        enableInfo: false,
        enableWarn: true,
        enableError: true,
      })
    } catch {
      /* ignore */
    }
    bindFlvChunkSource(this.source)
    this.player = flvjs.createPlayer(
      { type: 'flv', isLive: true, url: 'wt://moq-pull' },
      {
        enableStashBuffer: false,
        stashInitialSize: 128,
        lazyLoad: false,
        deferLoadAfterSourceOpen: false,
        fixAudioTimestampGap: false,
        autoCleanupSourceBuffer: true,
        autoCleanupMaxBackwardDuration: 8,
        autoCleanupMinBackwardDuration: 4,
        customLoader: FlvChunkLoader as never,
      },
    )
    this.player.on(flvjs.Events.ERROR, (type: unknown, detail: unknown, info: unknown) => {
      log_error('flv.js', `ERROR type=${String(type)} detail=${String(detail)}`, info)
    })
    this.player.on(flvjs.Events.MEDIA_INFO, (info: unknown) => {
      log_info('flv.js', 'MEDIA_INFO', info)
    })
    this.bindVideoEvents(video)
    video.muted = false
    video.volume = 1
    this.player.attachMediaElement(video)
    this.player.load()
    log_info('moq-pull', 'flv.js load()')
    void this.player.play()?.then(() => {
      video.muted = false
      video.volume = 1
    }).catch((e: unknown) => {
      log_warn('moq-pull', `play rejected: ${e instanceof Error ? e.message : String(e)}`)
    })
  }

  private bufferRanges(): BufRange[] {
    const v = this.videoEl
    if (!v) return []
    const out: BufRange[] = []
    for (let i = 0; i < v.buffered.length; i++) {
      out.push({ start: v.buffered.start(i), end: v.buffered.end(i) })
    }
    return out
  }

  private bufferLog(): string {
    const v = this.videoEl
    if (!v) return ''
    const ranges = this.bufferRanges()
    const text = ranges.map((r) => `[${r.start.toFixed(2)},${r.end.toFixed(2)}]`).join(' ')
    const end = ranges.length ? ranges[ranges.length - 1].end : 0
    const lag = ranges.length ? end - v.currentTime : 0
    return `t=${v.currentTime.toFixed(2)} lag=${lag.toFixed(2)} ready=${v.readyState} paused=${v.paused} buf=${text || '-'}`
  }

  private rangeContaining(t: number, ranges: BufRange[]): BufRange | null {
    for (const r of ranges) {
      if (t >= r.start - 0.02 && t < r.end) return r
    }
    return null
  }

  private seekInBuffer(target: number, reason: string) {
    const v = this.videoEl
    const p = this.player
    const ranges = this.bufferRanges()
    if (!v || !p || ranges.length === 0) return
    let hit = this.rangeContaining(target, ranges)
    if (!hit) {
      hit = ranges.find((r) => r.start > target) ?? ranges[ranges.length - 1]
      target = hit.start + 0.04
    }
    const maxT = Math.max(hit.start, hit.end - 0.08)
    if (target > maxT) target = maxT
    if (Math.abs(v.currentTime - target) < 0.05) return
    const now = performance.now()
    if (now - this.lastSeekMs < 400) return
    this.lastSeekMs = now
    log_info('moq-pull', `seek ${reason} -> ${target.toFixed(2)} ${this.bufferLog()}`)
    p.currentTime = target
    void v.play()?.catch(() => {})
  }

  private resumeTransmuxer() {
    const p = this.player as unknown as { _transmuxer?: { resume?: () => void } } | null
    try { p?._transmuxer?.resume?.() } catch { /* ignore */ }
  }

  private chaseLive(reason: string) {
    const v = this.videoEl
    if (!v) return
    const ranges = this.bufferRanges()
    if (ranges.length === 0) return
    const last = ranges[ranges.length - 1]
    const t = v.currentTime
    const here = this.rangeContaining(t, ranges)
    if (!here) {
      this.resumeTransmuxer()
      const next = ranges.find((r) => r.start > t)
      if (next) this.seekInBuffer(next.start + 0.04, `${reason}:hole`)
      else if (t < last.start) this.seekInBuffer(last.start + 0.04, `${reason}:behind`)
      /* 已越过 buffer 末端：等新数据，不要反复 seek 把自己打成 waiting 循环 */
      return
    }
    const lag = last.end - t
    if (lag > MAX_LATENCY_S) this.seekInBuffer(last.end - LIVE_LATENCY_S, `${reason}:lag=${lag.toFixed(2)}`)
  }

  private bindVideoEvents(video: HTMLVideoElement) {
    video.addEventListener('error', this.onVideoError)
    video.addEventListener('playing', this.onVideoPlaying)
    video.addEventListener('waiting', this.onVideoWaiting)
    video.addEventListener('stalled', this.onVideoStalled)
    video.addEventListener('loadedmetadata', this.onLoadedMeta)
  }

  private unbindVideoEvents() {
    const v = this.videoEl
    if (!v) return
    v.removeEventListener('error', this.onVideoError)
    v.removeEventListener('playing', this.onVideoPlaying)
    v.removeEventListener('waiting', this.onVideoWaiting)
    v.removeEventListener('stalled', this.onVideoStalled)
    v.removeEventListener('loadedmetadata', this.onLoadedMeta)
  }
}
