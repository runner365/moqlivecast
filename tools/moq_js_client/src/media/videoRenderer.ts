import { log_error, log_info, log_warn } from '../logger'

/**
 * WebGL2 视频渲染器。
 *
 * - 解码出的 VideoFrame 直接 `texImage2D` 上传（浏览器内部做 YUV→RGB，不需要手写转换）
 * - 帧按 timestamp（微秒）**有序插入**队列，rAF 里按媒体时钟弹出渲染
 * - 每帧都重绘：WebGL drawing buffer 在合成后会被清空，不能靠"不画就保留上一帧"
 *   （也不开 preserveDrawingBuffer，那有性能代价）
 * - 队列过深 / 落后时钟过多时丢帧（直播场景丢旧帧保实时）
 */
export type RenderFrame = TexImageSource & { close?: () => void }

export type VideoRendererStats = {
  received: number
  rendered: number
  dropped: number
  /** 当前队列深度 */
  queued: number
}

/** 目标直播缓冲（微秒）：队列只保留"最新帧往前这么久"的帧。
 *  太大 → 落后 live 多（订阅时 GOP 回放的旧数据全攒着）；
 *  太小 → 网络抖动时容易空队列。
 *  取 300ms 与音频环形缓冲同量级，使音画延迟相当。 */
const LIVE_TARGET_US = 300_000

/** 队列帧数硬上限：纯防御性兜底，正常不该触达（15fps 下 600 帧 ≈ 40s）。
 *  **不能用"相对最新帧的时间窗"裁剪** —— 视频数据通常超前时钟，
 *  那样会把时钟正要渲染的那帧也丢掉，画面会永久冻住。
 *  真正该丢的（时钟已越过的）由 renderTick 的 MAX_LAG_US 按【时钟】判断。 */
const MAX_QUEUE_FRAMES = 600
/** 队首落后媒体时钟超过这个值就一次性丢到最新（替代 MSE 的 seek 追帧） */
const MAX_LAG_US = 400_000
/** 数据超前时钟超过这个值 → 判定为追帧突发（GOP 回放/服务端快发），重锚时钟到直播边缘。
 *  不重锚的话时钟以 1x 永远追不上积压，可渲染的帧会被队列溢出丢光 → 画面冻住。 */
const MAX_AHEAD_US = 500_000
/** 重锚时保留的缓冲（让直播边缘略落后于最新帧，留一点平滑余量） */
const LIVE_BUFFER_US = 200_000

const VERT_SRC = `#version 300 es
layout(location = 0) in vec2 a_pos;
layout(location = 1) in vec2 a_uv;
out vec2 v_uv;
void main() {
  v_uv = a_uv;
  gl_Position = vec4(a_pos, 0.0, 1.0);
}`

const FRAG_SRC = `#version 300 es
precision mediump float;
in vec2 v_uv;
uniform sampler2D u_tex;
out vec4 outColor;
void main() {
  outColor = texture(u_tex, v_uv);
}`

/* 全屏 quad（triangle strip）。注意 v 翻转：
 * VideoFrame 是左上原点，WebGL 纹理坐标是左下原点，这里直接在 uv 里翻。 */
const VERTS = new Float32Array([
  -1, -1, 0, 1,
  1, -1, 1, 1,
  -1, 1, 0, 0,
  1, 1, 1, 0,
])

type QueuedFrame = { tsUs: number; frame: RenderFrame }

export class VideoRenderer {
  private readonly canvas: HTMLCanvasElement
  private readonly gl: WebGL2RenderingContext | null
  private program: WebGLProgram | null = null
  private texture: WebGLTexture | null = null
  private vao: WebGLVertexArrayObject | null = null
  private vbo: WebGLBuffer | null = null
  private readonly uTexLoc: WebGLUniformLocation | null = null

  private queue: QueuedFrame[] = []
  private rafId = 0
  private closed = false
  private hasFrame = false

  /** 媒体时钟（微秒）。
   *  默认实现是"锚定时钟"：第一帧到达时把它的 ts 锚到当前墙钟，之后按 1x 前进。
   *  这样帧 ts 从 0 开始也能对上（直接用 performance.now()*1000 会让每帧都立即到期）。
   *  第 5 步接入音频后，会换成 AudioWorklet 回传的真实消费帧数（避免漂移）。 */
  private externalClock: (() => number) | null = null
  private anchorTsUs = -1
  private anchorWallMs = 0

  private stats: VideoRendererStats = { received: 0, rendered: 0, dropped: 0, queued: 0 }
  private firstFrameLogged = false
  private queueDropLogged = 0

  constructor(canvas: HTMLCanvasElement) {
    this.canvas = canvas
    this.gl = canvas.getContext('webgl2', { alpha: false })
    if (!this.gl) {
      log_error('wc-render', 'init FAILED: WebGL2 not available (浏览器/显卡不支持)')
      return
    }
    this.program = this.buildProgram()
    if (!this.program) {
      log_error('wc-render', 'init FAILED: shader program build/link failed')
      return
    }
    this.uTexLoc = this.gl.getUniformLocation(this.program, 'u_tex')
    this.texture = this.createTexture()
    this.vao = this.createQuad()
    if (!this.texture || !this.vao) {
      log_error(
        'wc-render',
        `init FAILED: texture=${this.texture ? 'ok' : 'null'} vao=${this.vao ? 'ok' : 'null'}`,
      )
      return
    }
    log_info('wc-render', `init ok size=${canvas.width}x${canvas.height}`)
  }

  /** 外部时钟（微秒）。第 5 步用音频时钟覆盖锚定时钟。 */
  setClock(fn: () => number) {
    this.externalClock = fn
  }

  private clock(): number {
    /* 外部时钟（音频）优先；不可用（-1）时回退到内部锚定时钟，
     * 否则音频起播前视频会一帧都渲染不出来。 */
    if (this.externalClock) {
      const t = this.externalClock()
      if (t >= 0) return t
    }
    if (this.anchorTsUs < 0) return -1 /* 还没锚定：不渲染任何帧 */
    return this.anchorTsUs + (performance.now() - this.anchorWallMs) * 1000
  }

  get ready(): boolean {
    return !!this.gl && !!this.program && !!this.texture && !!this.vao
  }

  getStats(): VideoRendererStats {
    return { ...this.stats, queued: this.queue.length }
  }

  start() {
    if (this.closed) {
      log_warn('wc-render', 'start ignored: already closed')
      return
    }
    if (this.rafId) return
    if (!this.ready) {
      log_error('wc-render', 'start FAILED: renderer not ready (init 失败)')
      return
    }
    const tick = () => {
      if (this.closed) return
      this.renderTick()
      this.rafId = window.requestAnimationFrame(tick)
    }
    this.rafId = window.requestAnimationFrame(tick)
    log_info('wc-render', 'render loop started (rAF)')
  }

  /** 入队一帧（tsUs 微秒）。队列按 ts 升序。 */
  push(tsUs: number, frame: RenderFrame) {
    if (this.closed || !this.ready) {
      log_error('wc-render', `push dropped: closed=${this.closed} ready=${this.ready}`)
      closeFrame(frame)
      return
    }
    this.stats.received++
    if (!this.firstFrameLogged) {
      this.firstFrameLogged = true
      log_info('wc-render', `first frame queued ts=${tsUs}us`)
    }
    /* 第一帧到达即锚定内部时钟。
     * 即使有外部时钟（音频）也要锚 —— 音频未起播时外部时钟返回 -1，
     * 此时必须能回退到锚定时钟，否则一帧都渲染不出来。 */
    if (this.anchorTsUs < 0) {
      this.anchorTsUs = tsUs
      this.anchorWallMs = performance.now()
      log_info('wc-render', `clock anchored at ts=${tsUs}us`)
    }
    const q = this.queue
    let i = q.length
    while (i > 0 && q[i - 1].tsUs > tsUs) i--
    q.splice(i, 0, { tsUs, frame })

    /* 只做硬上限兜底：真正的裁剪按【时钟】做（renderTick 里），
     * 因为"相对最新帧"裁剪会在数据超前时钟时把时钟正要用的帧丢光。 */
    while (q.length > MAX_QUEUE_FRAMES) {
      const drop = q.shift()
      if (!drop) break
      closeFrame(drop.frame)
      this.stats.dropped++
      this.queueDropLogged++
      if (this.queueDropLogged <= 3 || this.queueDropLogged % 120 === 0) {
        log_warn(
          'wc-render',
          `queue hard cap (${MAX_QUEUE_FRAMES}) exceeded, drop oldest ts=${Math.round(drop.tsUs / 1000)}ms ` +
            `(#${this.queueDropLogged})`,
        )
      }
    }
  }

  /**
   * 画一张测试图案（彩条 + 中心方块）。
   * 用途：接通真解码器之前先验证 GL 通路 — 黑屏时能立刻区分「GL 坏了」还是「没解码出帧」。
   */
  async renderTestPattern() {
    if (this.closed || !this.ready) return
    const w = 640
    const h = 360
    const c = document.createElement('canvas')
    c.width = w
    c.height = h
    const ctx = c.getContext('2d')
    if (!ctx) return
    const bars = ['#c0c0c0', '#c0c000', '#00c0c0', '#00c000', '#c000c0', '#c00000', '#0000c0', '#000000']
    const bw = w / bars.length
    for (let i = 0; i < bars.length; i++) {
      ctx.fillStyle = bars[i]
      ctx.fillRect(i * bw, 0, bw + 1, h)
    }
    ctx.fillStyle = '#1f6feb'
    ctx.fillRect(w / 2 - 80, h / 2 - 45, 160, 90)
    ctx.fillStyle = '#fff'
    ctx.font = 'bold 22px monospace'
    ctx.textAlign = 'center'
    ctx.textBaseline = 'middle'
    ctx.fillText('WebCodecs GL OK', w / 2, h / 2)
    try {
      const bmp = await createImageBitmap(c)
      if (this.closed) {
        bmp.close()
        return
      }
      this.upload(bmp)
      bmp.close()
      this.hasFrame = true
      this.draw()
      log_info('wc-render', 'test pattern drawn')
    } catch (e) {
      log_warn('wc-render', `test pattern failed: ${e instanceof Error ? e.message : String(e)}`)
    }
  }

  private lastDiagWallMs = 0

  private renderTick() {
    if (!this.hasFrame && this.queue.length === 0) return
    let nowUs = this.clock()

    /* 每秒一条状态：直接看 clock 与队列头尾的关系，判断"为什么没渲染" */
    const wall = performance.now()
    if (wall - this.lastDiagWallMs >= 1000) {
      this.lastDiagWallMs = wall
      const head = this.queue.length ? this.queue[0].tsUs : -1
      const tail = this.queue.length ? this.queue[this.queue.length - 1].tsUs : -1
      log_info(
        'wc-render',
        `tick now=${Math.round(nowUs / 1000)}ms head=${Math.round(head / 1000)}ms ` +
          `tail=${Math.round(tail / 1000)}ms queued=${this.queue.length} ` +
          `rendered=${this.stats.rendered} dropped=${this.stats.dropped} ` +
          `got=${this.stats.received} ext=${this.externalClock ? 'audio' : 'anchor'}`,
      )
    }
    /* 时钟未就绪（还没锚定、也没外部时钟）：保留帧，先不渲染 */
    if (nowUs < 0) {
      if (this.hasFrame) this.draw()
      return
    }

    /* 追直播：数据超前时钟太多 → 重锚时钟到直播边缘。
     * 音频时钟可用时以音频为准（不重锚，否则会撕裂音画同步）；
     * 回退到锚定时钟时才重锚。 */
    const usingAnchorClock = !this.externalClock || this.externalClock() < 0
    if (usingAnchorClock && this.queue.length > 0) {
      const newest = this.queue[this.queue.length - 1]
      if (newest.tsUs - nowUs > MAX_AHEAD_US) {
        const jumpMs = Math.round((newest.tsUs - nowUs) / 1000)
        this.anchorTsUs = newest.tsUs - LIVE_BUFFER_US
        this.anchorWallMs = performance.now()
        nowUs = this.clock() /* 重锚后时钟变了，后续判定要用新值 */
        log_warn(
          'wc-render',
          `clock re-anchored +${jumpMs}ms ahead (live edge, buffered=${this.queue.length})`,
        )
      }
    }

    /* 唯一需要的裁剪：丢掉【时钟已经越过太久】的过期帧（防无限积压）。
     *
     * 不要按「最新帧」裁剪 —— 数据超前时钟是正常的（服务端快发/GOP 回放），
     * 那时该做的是"等时钟走过来"，而不是把时钟正要用的帧丢掉。
     * 保留至少 1 帧，避免队列清空后无内容可画。 */
    while (this.queue.length > 1 && nowUs - this.queue[0].tsUs > MAX_LAG_US) {
      const f = this.queue.shift()
      if (!f) break
      closeFrame(f.frame)
      this.stats.dropped++
    }

    /* 弹出所有到点的帧，只渲染最后一个 */
    let latest: QueuedFrame | null = null
    while (this.queue.length > 0 && this.queue[0].tsUs <= nowUs) {
      const f = this.queue.shift()
      if (!f) break
      if (latest) {
        closeFrame(latest.frame)
        this.stats.dropped++
      }
      latest = f
    }

    if (latest) {
      const ok = this.upload(latest.frame)
      if (ok) {
        this.hasFrame = true
        this.stats.rendered++
        if (this.stats.rendered === 1) {
          log_info('wc-render', `first frame rendered ts=${latest.tsUs}us (画面已出)`)
        }
      }
      closeFrame(latest.frame)
    }

    /* 每帧都重绘（drawing buffer 合成后会被清空） */
    if (this.hasFrame) this.draw()
  }

  /** 上传纹理。返回是否成功。 */
  private upload(src: TexImageSource): boolean {
    const gl = this.gl
    if (!gl || !this.texture) {
      log_error('wc-render', 'upload skipped: gl/texture null')
      return false
    }
    /* 分辨率变化时同步 canvas 尺寸 */
    const w = srcWidth(src) || this.canvas.width
    const h = srcHeight(src) || this.canvas.height
    if (w > 0 && h > 0 && (this.canvas.width !== w || this.canvas.height !== h)) {
      this.canvas.width = w
      this.canvas.height = h
      gl.viewport(0, 0, w, h)
      log_info('wc-render', `canvas resized -> ${w}x${h}`)
    }
    try {
      gl.bindTexture(gl.TEXTURE_2D, this.texture)
      gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, gl.RGBA, gl.UNSIGNED_BYTE, src)
      return true
    } catch (e) {
      log_error('wc-render', `texImage2D failed: ${e instanceof Error ? e.message : String(e)}`)
      return false
    }
  }

  private draw() {
    const gl = this.gl
    if (!gl || !this.program || !this.vao || !this.texture) return
    gl.useProgram(this.program)
    gl.bindVertexArray(this.vao)
    gl.activeTexture(gl.TEXTURE0)
    gl.bindTexture(gl.TEXTURE_2D, this.texture)
    if (this.uTexLoc) gl.uniform1i(this.uTexLoc, 0)
    gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4)
  }

  close() {
    if (this.closed) return
    this.closed = true
    if (this.rafId) {
      window.cancelAnimationFrame(this.rafId)
      this.rafId = 0
    }
    for (const f of this.queue) closeFrame(f.frame)
    this.queue = []
    const gl = this.gl
    if (gl) {
      if (this.texture) gl.deleteTexture(this.texture)
      if (this.vbo) gl.deleteBuffer(this.vbo)
      if (this.vao) gl.deleteVertexArray(this.vao)
      if (this.program) gl.deleteProgram(this.program)
    }
    this.texture = null
    this.vbo = null
    this.vao = null
    this.program = null
    log_info('wc-render', `close stats=${JSON.stringify(this.stats)}`)
  }

  private buildProgram(): WebGLProgram | null {
    const gl = this.gl
    if (!gl) return null
    const vs = compile(gl, gl.VERTEX_SHADER, VERT_SRC)
    const fs = compile(gl, gl.FRAGMENT_SHADER, FRAG_SRC)
    if (!vs || !fs) return null
    const prog = gl.createProgram()
    if (!prog) return null
    gl.attachShader(prog, vs)
    gl.attachShader(prog, fs)
    gl.linkProgram(prog)
    gl.deleteShader(vs)
    gl.deleteShader(fs)
    if (!gl.getProgramParameter(prog, gl.LINK_STATUS)) {
      log_error('wc-render', `link failed: ${gl.getProgramInfoLog(prog)}`)
      gl.deleteProgram(prog)
      return null
    }
    return prog
  }

  private createTexture(): WebGLTexture | null {
    const gl = this.gl
    if (!gl) return null
    const tex = gl.createTexture()
    gl.bindTexture(gl.TEXTURE_2D, tex)
    /* CLAMP + LINEAR：视频尺寸非 2 的幂也合法 */
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE)
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE)
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR)
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR)
    return tex
  }

  private createQuad(): WebGLVertexArrayObject | null {
    const gl = this.gl
    if (!gl) return null
    const vao = gl.createVertexArray()
    const vbo = gl.createBuffer()
    if (!vao || !vbo) return null
    gl.bindVertexArray(vao)
    gl.bindBuffer(gl.ARRAY_BUFFER, vbo)
    gl.bufferData(gl.ARRAY_BUFFER, VERTS, gl.STATIC_DRAW)
    gl.enableVertexAttribArray(0)
    gl.vertexAttribPointer(0, 2, gl.FLOAT, false, 16, 0)
    gl.enableVertexAttribArray(1)
    gl.vertexAttribPointer(1, 2, gl.FLOAT, false, 16, 8)
    gl.bindVertexArray(null)
    this.vbo = vbo
    return vao
  }
}

function compile(gl: WebGL2RenderingContext, type: number, src: string): WebGLShader | null {
  const sh = gl.createShader(type)
  if (!sh) return null
  gl.shaderSource(sh, src)
  gl.compileShader(sh)
  if (!gl.getShaderParameter(sh, gl.COMPILE_STATUS)) {
    log_error('wc-render', `shader compile failed: ${gl.getShaderInfoLog(sh)}`)
    gl.deleteShader(sh)
    return null
  }
  return sh
}

function closeFrame(f: RenderFrame) {
  try {
    f.close?.()
  } catch {
    /* ignore */
  }
}

function srcWidth(src: TexImageSource): number {
  const f = src as Partial<VideoFrame>
  if (typeof f.displayWidth === 'number' && f.displayWidth > 0) return f.displayWidth
  if (typeof f.codedWidth === 'number' && f.codedWidth > 0) return f.codedWidth
  const b = src as Partial<ImageBitmap>
  return typeof b.width === 'number' ? b.width : 0
}

function srcHeight(src: TexImageSource): number {
  const f = src as Partial<VideoFrame>
  if (typeof f.displayHeight === 'number' && f.displayHeight > 0) return f.displayHeight
  if (typeof f.codedHeight === 'number' && f.codedHeight > 0) return f.codedHeight
  const b = src as Partial<ImageBitmap>
  return typeof b.height === 'number' ? b.height : 0
}
