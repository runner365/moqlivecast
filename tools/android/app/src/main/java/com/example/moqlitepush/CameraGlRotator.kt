package com.example.moqlitepush

import android.graphics.SurfaceTexture
import android.opengl.EGL14
import android.opengl.EGLConfig
import android.opengl.EGLContext
import android.opengl.EGLDisplay
import android.opengl.EGLExt
import android.opengl.EGLSurface
import android.opengl.GLES11Ext
import android.opengl.GLES20
import android.os.Handler
import android.os.HandlerThread
import android.util.Log
import android.view.Surface
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.nio.FloatBuffer

/**
 * 相机帧的**转正**（旋转 + 可选镜像），夹在相机和编码器之间。
 *
 *     Camera2 ──SurfaceTexture(OES)──> GL shader ──┬──> 编码器输入 Surface
 *                                                  └──> 预览 Surface（可选）
 *
 * ⚠️ **为什么非要有这一层**：相机走 Surface 时**不会**帮你摆正画面 —— 传感器怎么
 * 装的，吐出来就是怎么样的。实测这台设备前摄 `SENSOR_ORIENTATION=270`：
 * 编出来的码流里装的是**横躺**的画面，于是
 *   - 手机预览里，4:3 横框里塞着一个竖着的场景 → 看起来「竖向被压扁」；
 *   - web 端 `frame geometry … rotation=0`（码流里没有任何方向元数据）→ 原样渲染
 *     → 看起来「转了 90°」。
 * MediaFormat.KEY_ROTATION 救不了：它只写容器元数据，而我们是裸 H.264，
 * 没有容器可写。所以像素必须在**进编码器之前**转过来 —— 就是这一层。
 *
 * 顺带解决的两件事：
 *   ① 相机只剩这**一个**输出（`inputSurface`），预览改由 GL 画 —— 于是预览不再受
 *      「必须是相机支持的尺寸」约束，也就不存在「预览 buffer 比例和视图形状对不上
 *      被拉变形」的问题了（那正是之前预览被压扁的另一半原因）。
 *   ② 旋转 90/270 时画面的**宽高比会互换**（640x480 转正后是 480x640 的竖构图），
 *      预览按转正后的比例做信箱式留边，编码器那边则原样铺满。
 */
class CameraGlRotator(private val onLog: (String) -> Unit) {

    companion object {
        private const val TAG = "MoqGl"

        /**
         * 方向兜底微调（度，0/90/180/270）。
         *
         * 厂家对前置传感器的**读取方向**并不统一（有的读出来就是镜像的），
         * 这种事在真机上一眼看得出来，却没法靠读代码断定 —— 所以留一个旋钮。
         *
         * ⚠️ 默认值 **270 是本机实测出来的**：前摄 sensor=270，而实测「转正」需要
         * 的总角度是 180°（= (270 + 270) % 360），也就是这里要补 270。
         * 也就是说**这台设备的前摄读出来比 SENSOR_ORIENTATION 说的少 90°**
         * （厂家对前置传感器的读取方向不统一，这种事只能靠真机看）。
         * 换设备/换摄像头时把这个值按下面那条规则重新试。
         *
         * 按**症状**选值，不用试遍：
         *   - 画面上下颠倒，**或者**「像是朝另一边转了 90°」→ 下一次 +90 就对了（+180）
         *     （这两者其实是同一个错误：顺着转 90° 和逆着转 90° 差的正是 180°）
         *   - 画面是镜像的（字是反的）→ 打开 [mirrorFront]
         * 只动这里就够了：编码栅格的宽高会跟着重新推（90/270 会换成竖的），
         * 不需要另外改尺寸。
         */
        @Volatile var rotationAdjustDeg = 270
            private set

        /** 循环 0 → 90 → 180 → 270（每次 +90）。用来在真机上**当场**把方向试出来，
         *  不必为了一个常量反复重编。生效需要重起一次采集（栅格宽高会跟着换），
         *  所以调用方改完要 restart。 */
        fun cycleRotationAdjust(): Int {
            rotationAdjustDeg = (rotationAdjustDeg + 90) % 360
            return rotationAdjustDeg
        }

        /** 最近一次算出来的**实际**顺时针角度，只用于显示 —— 它 = sensor-display+adjust。 */
        @Volatile var lastRotationDeg = 0
            private set

        /**
         * 前置是否做水平镜像。**纯观感**，不影响「能不能看」：
         *  - true  = 自拍视角（像照镜子，字会反）
         *  - false = 真实场景（推给观众看更合适，默认这个）
         */
        @Volatile var mirrorFront = false
            private set

        /** 前置镜像开关（自拍视角）。和旋转一样做成运行时可切，便于一次把方向试对。 */
        fun toggleMirrorFront(): Boolean {
            mirrorFront = !mirrorFront
            return mirrorFront
        }

        /**
         * 画面转正需要**顺时针**转多少度、要不要镜像。
         *
         * ⚠️ **编码尺寸必须跟着这个结果换宽高**：相机给的是横向的 640x480，
         * 转正后的画面却是**竖的** 480x640。只把内容转过来、仍编进 640x480 的栅格，
         * 等于把竖构图横向拉满 —— 失真只是从手机屏搬到播放端而已。所以这个结果
         * 要拿去做两件事：转 GL，以及把**编码器/取景的宽高互换**。
         *
         * 转正需要的**顺时针**角度 = `sensor - display`，**前后摄一样**。
         *
         * ⚠️ 别照搬 Camera1 `setDisplayOrientation` 的前摄公式 —— 那边最后多一步
         * `(360 - result)`，是因为**驱动已经把预览镜像过了**，必须反号抵消回去。
         * 我们走的是 Camera2 的 Surface，链路上**没有任何镜像**，照搬会平白多转
         * 180°：本机前摄 sensor=270，按那边算镜像时是 90°，而「镜像 + 转正」正确
         * 的做法是 M ∘ R(270)，两者正好差 180°。
         *
         * 镜像在这条链路上只是**观感**（自拍视角），不该参与角度计算 —— 它是转正
         * **之后**整体翻一下，也就是 start() 里对顶点 x 取反那一步。
         * 真机上方向不对就动 [rotationAdjustDeg] 和 [mirrorFront] 这两个旋钮。
         *
         * @return (顺时针角度 0/90/180/270, 是否水平镜像)
         */
        fun rotationFor(sensorOrientation: Int, displayDeg: Int, frontFacing: Boolean): Pair<Int, Boolean> {
            val deg = ((sensorOrientation - displayDeg) % 360 + 360) % 360
            val rot = (((deg + rotationAdjustDeg) % 360) + 360) % 360
            lastRotationDeg = rot
            return rot to (frontFacing && mirrorFront)
        }
    }

    /** 相机要写进来的那块 Surface。相机 session 只挂它一个输出。 */
    @Volatile var inputSurface: Surface? = null
        private set

    private var thread: HandlerThread? = null
    private var handler: Handler? = null

    // ── 以下只在 GL 线程上碰 ──
    private var eglDisplay: EGLDisplay = EGL14.EGL_NO_DISPLAY
    private var eglContext: EGLContext = EGL14.EGL_NO_CONTEXT
    private lateinit var eglConfig: EGLConfig
    private var encoderWindow: EGLSurface = EGL14.EGL_NO_SURFACE
    private var previewWindow: EGLSurface = EGL14.EGL_NO_SURFACE

    private var textureId = 0
    private var surfaceTexture: SurfaceTexture? = null
    private var program = 0
    private var uSTMatrixLoc = 0
    private var aPositionLoc = 0
    private var aTexCoordLoc = 0
    private var quadPos: FloatBuffer? = null
    private val stMatrix = FloatArray(16)

    /** 编码栅格尺寸（start() 里按旋转推出来）。 */
    private var encW = 0
    private var encH = 0
    /* 相机输出的尺寸。⚠️ 必须是**字段**而不是 start() 的参数：initGl() 跑在 GL 线程上、
     * 由 handler.post 稍后才执行，那时候参数早出作用域了。 */
    private var camW = 0
    private var camH = 0
    private var frameCount = 0
    /** initGl 的完成信号（成功与否都 countDown），[awaitReady] 等它。 */
    private var initLatch = java.util.concurrent.CountDownLatch(1)
    @Volatile private var initOk = false

    /** 预览的 Surface 可能晚于 start() 出现，也可能中途换（SurfaceView 销毁重建）。
     *  任意线程可调。 */
    fun setPreviewSurface(s: Surface?) {
        handler?.post { setPreviewSurfaceOnGl(s) }
    }

    /** 转正后画面的宽高比。旋转 90/270 时它会和编码宽高比互为倒数 ——
     *  预览视图要按**这个**来摆（640x480 转正后是竖的 480x640）。 */
    @Volatile var displayAspect: Float = 4f / 3f
        private set

    /**
     * 起 GL 线程并把编码器的输入 Surface 接管过来。
     *
     * @param cameraW,cameraH **相机输出**的尺寸（相机只给横向的，如 640x480）——
     *   相机写进 OES 纹理的就是这么大。
     * @param rotationDegrees,mirror 由 [rotationFor] 算好（含兜底微调）。
     *
     * ⚠️ 编码栅格的尺寸在这里由 `rot` **推**出来（转 90/270 就换宽高），不另传 ——
     * 两处各算一遍迟早会对不上。相机那块纹理仍按 cameraW×cameraH 建，编码器则是
     * 竖的 encW×encH：转正后的画面本来就是竖的，编进横向栅格会被横向拉满。
     */
    fun start(encoderSurface: Surface, cameraW: Int, cameraH: Int,
              rotationDegrees: Int, mirror: Boolean) {
        camW = cameraW
        camH = cameraH
        initLatch = java.util.concurrent.CountDownLatch(1)
        initOk = false
        val rot = ((rotationDegrees % 360) + 360) % 360
        val swap = rot % 180 == 90
        encW = if (swap) cameraH else cameraW
        encH = if (swap) cameraW else cameraH
        displayAspect = encW.toFloat() / encH
        val k = rot / 90
        /* 顶点按角度换位置（而不是在 shader 里乘旋转矩阵）—— 这样 SurfaceTexture
         * 自带的那张 uSTMatrix 仍然作用在**未旋转**的纹理坐标上，两者不会打架。
         * 屏幕坐标 y 向上，所以「顺时针 90°」是 (x,y) → (y,-x)。 */
        val corners = floatArrayOf(-1f, -1f, 1f, -1f, -1f, 1f, 1f, 1f)
        val pos = FloatArray(8)
        for (i in 0 until 4) {
            var x = corners[i * 2]
            var y = corners[i * 2 + 1]
            when (k) {
                1 -> { val t = x; x = y; y = -t }
                2 -> { x = -x; y = -y }
                3 -> { val t = x; x = -y; y = t }
            }
            if (mirror) x = -x
            pos[i * 2] = x
            pos[i * 2 + 1] = y
        }
        quadPos = ByteBuffer.allocateDirect(pos.size * 4)
            .order(ByteOrder.nativeOrder()).asFloatBuffer().apply { put(pos); position(0) }

        onLog("gl rotator: rotation=$rot° mirror=$mirror → encode raster " +
              "${encW}x${encH} (aspect %.3f)".format(displayAspect))

        thread = HandlerThread("moq-gl").also { it.start() }
        handler = Handler(thread!!.looper)
        handler!!.post { initGl(encoderSurface) }
    }

    /** 等 GL 层把 Surface 建出来，最多 [timeoutMs] 毫秒；返回是否**成功**建好。
     *
     * ⚠️ 相机必须等它。相机的输出就是这块 Surface，而 GL 是异步起的
     * （`start()` 只是往 GL 线程 post 一个任务就返回），不等就会在 createSession
     * 里读到 null —— 实测就是这样：相机 320ms 就开好了，GL 要 635ms，
     * 于是 session 根本建不起来，预览和视频一起消失。
     *
     * ⚠️ 别在**主线程**上等（createSession 跑在 cameraHandler 上，所以它等得起）。 */
    fun awaitReady(timeoutMs: Long): Boolean = try {
        initLatch.await(timeoutMs, java.util.concurrent.TimeUnit.MILLISECONDS) && initOk
    } catch (_: InterruptedException) {
        false
    }

    fun stop() {
        val h = handler
        val t = thread
        if (h != null && t != null && t.isAlive) {
            val done = java.util.concurrent.CountDownLatch(1)
            h.post { releaseGl(); done.countDown() }
            done.await(1000, java.util.concurrent.TimeUnit.MILLISECONDS)
        }
        thread?.quitSafely()
        thread = null
        handler = null
        inputSurface = null
    }

    // ── GL 线程 ────────────────────────────────────────────────

    private fun initGl(encoderSurface: Surface) {
        val h = handler ?: run { initLatch.countDown(); return }
        try {
            eglDisplay = EGL14.eglGetDisplay(EGL14.EGL_DEFAULT_DISPLAY)
            val ver = IntArray(2)
            EGL14.eglInitialize(eglDisplay, ver, 0, ver, 1)

            val cfgAttr = intArrayOf(
                EGL14.EGL_RED_SIZE, 8, EGL14.EGL_GREEN_SIZE, 8,
                EGL14.EGL_BLUE_SIZE, 8, EGL14.EGL_ALPHA_SIZE, 8,
                EGL14.EGL_RENDERABLE_TYPE, EGL14.EGL_OPENGL_ES2_BIT,
                /* ⚠️ 必须要 RECORDABLE：编码器的输入 Surface 是消费者，EGL 得能按帧
                 * 提交（配合 eglPresentationTimeANDROID 把相机的时间戳传下去）。 */
                EGLExt.EGL_RECORDABLE_ANDROID, 1,
                EGL14.EGL_NONE)
            val cfgs = arrayOfNulls<EGLConfig>(1)
            val n = IntArray(1)
            if (!EGL14.eglChooseConfig(eglDisplay, cfgAttr, 0, cfgs, 0, 1, n, 0) || n[0] < 1) {
                throw IllegalStateException("eglChooseConfig failed")
            }
            /* cfgs 是 Array<EGLConfig?>（那个 API 就这么声明的），但 n[0] >= 1 已经
             * 保证 [0] 有值 —— 显式挡一下，别用 !! 把这份保证变成一句口头承诺。 */
            eglConfig = cfgs[0] ?: throw IllegalStateException("eglChooseConfig returned null")

            eglContext = EGL14.eglCreateContext(eglDisplay, eglConfig, EGL14.EGL_NO_CONTEXT,
                intArrayOf(EGL14.EGL_CONTEXT_CLIENT_VERSION, 2, EGL14.EGL_NONE), 0)
            checkEgl("eglCreateContext")
            encoderWindow = EGL14.eglCreateWindowSurface(eglDisplay, eglConfig, encoderSurface,
                intArrayOf(EGL14.EGL_NONE), 0)
            checkEgl("eglCreateWindowSurface(encoder)")

            /* ⚠️ **必须先 makeCurrent，再做任何 GL 调用。** 没有当前上下文时
             * glCreateShader 返回 0、glCompileShader(0) 静默失败，而
             * glGetShaderInfoLog 只会给一个**空字符串** —— 于是报出来的错就是
             * 「compile failed: 」（后面什么都没有），完全看不出真正的原因。
             * 第一版就踩了这个坑：后面所有 GL 调用（建纹理、编译 shader）全废，
             * initGl 抛异常 → inputSurface 为 null → 相机 session 根本不建，
             * 表现是「预览和视频一起消失」。
             * 建纹理、编译 shader 之前都必须在**这个**上下文里。 */
            if (!EGL14.eglMakeCurrent(eglDisplay, encoderWindow, encoderWindow, eglContext)) {
                throw IllegalStateException("eglMakeCurrent failed")
            }

            val prog = buildProgram()
            program = prog
            uSTMatrixLoc = GLES20.glGetUniformLocation(program, "uSTMatrix")
            aPositionLoc = GLES20.glGetAttribLocation(program, "aPosition")
            aTexCoordLoc = GLES20.glGetAttribLocation(program, "aTexCoord")

            val tex = IntArray(1)
            GLES20.glGenTextures(1, tex, 0)
            textureId = tex[0]
            GLES20.glBindTexture(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, textureId)
            GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES,
                GLES20.GL_TEXTURE_MIN_FILTER, GLES20.GL_LINEAR)
            GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES,
                GLES20.GL_TEXTURE_MAG_FILTER, GLES20.GL_LINEAR)
            GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES,
                GLES20.GL_TEXTURE_WRAP_S, GLES20.GL_CLAMP_TO_EDGE)
            GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES,
                GLES20.GL_TEXTURE_WRAP_T, GLES20.GL_CLAMP_TO_EDGE)

            val st = SurfaceTexture(textureId)
            /* ⚠️ 这里必须是**相机**的尺寸（横向的）—— 相机只会给相机支持的尺寸，
             * 把纹理设成竖的 480x640 相机会建不出 session。转正是 GL 干的活。 */
            st.setDefaultBufferSize(camW, camH)
            st.setOnFrameAvailableListener({ drawFrame() }, h)
            surfaceTexture = st
            inputSurface = Surface(st)

            onLog("gl rotator ready: camera->${camW}x${camH} encode->${encW}x${encH}")
            initOk = true
        } catch (t: Throwable) {
            Log.e(TAG, "initGl failed", t)
            onLog("gl rotator init FAILED: ${t.message}")
        } finally {
            /* ⚠️ 成功失败都要放行 —— 只在成功路径 countDown 的话，失败时
             * createSession 会白白等满超时。 */
            initLatch.countDown()
        }
    }

    private fun drawFrame() {
        val st = surfaceTexture ?: return
        try {
            st.updateTexImage()
            st.getTransformMatrix(stMatrix)
            /* 编码器：整块铺满（栅格尺寸就是帧尺寸）。 */
            drawTo(encoderWindow, cover = false, ptsNs = st.timestamp)
            /* 预览：填满竖框，左右居中裁剪（见 drawTo 的 cover 分支）。 */
            previewWindow.takeIf { it != EGL14.EGL_NO_SURFACE }
                ?.let { drawTo(it, cover = true, ptsNs = 0) }
            frameCount++
            if (frameCount == 1) onLog("gl rotator: first frame passed through")
        } catch (t: Throwable) {
            Log.e(TAG, "drawFrame failed", t)
        }
    }

    private fun drawTo(window: EGLSurface, cover: Boolean, ptsNs: Long) {
        if (!EGL14.eglMakeCurrent(eglDisplay, window, window, eglContext)) return
        /* 尺寸一律**问 surface**，不用我们记的那个数：编码器的输入 Surface 是
         * MediaCodec 建的，它有权按自己的对齐取整；照着我们记的数字设 viewport
         * 就会错位或留边。 */
        val q = IntArray(1)
        EGL14.eglQuerySurface(eglDisplay, window, EGL14.EGL_WIDTH, q, 0)
        val vw = q[0]
        EGL14.eglQuerySurface(eglDisplay, window, EGL14.EGL_HEIGHT, q, 0)
        val vh = q[0]
        if (vw <= 0 || vh <= 0) return
        if (!cover) {
            /* 编码器：栅格和帧同比例，整块铺满即可。 */
            GLES20.glViewport(0, 0, vw, vh)
        } else {
            /* 预览：**填满**（cover），不是留边（contain）—— 按画面比例放大到刚好
             * 盖住整块 surface，多出来的被 framebuffer 裁掉，也就是**居中裁剪**。
             * 手机竖着拿、而 4:3 的横向画面塞进竖框时，裁掉的是左右两侧。
             * viewport 允许超出 surface（这里会是负偏移），超出部分天然被裁掉。 */
            var dw = vw; var dh = vh
            if (vw.toFloat() / vh > displayAspect) dh = (vw / displayAspect).toInt()
            else                                   dw = (vh * displayAspect).toInt()
            GLES20.glViewport((vw - dw) / 2, (vh - dh) / 2, dw, dh)
        }

        GLES20.glUseProgram(program)
        GLES20.glActiveTexture(GLES20.GL_TEXTURE0)
        GLES20.glBindTexture(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, textureId)
        GLES20.glUniformMatrix4fv(uSTMatrixLoc, 1, false, stMatrix, 0)

        val qp = quadPos ?: return
        qp.position(0)
        GLES20.glVertexAttribPointer(aPositionLoc, 2, GLES20.GL_FLOAT, false, 0, qp)
        GLES20.glEnableVertexAttribArray(aPositionLoc)
        /* 纹理坐标与顶点一一对应（左下→右下→左上→右上），v 朝下所以 v=1 是底边。 */
        texCoords.position(0)
        GLES20.glVertexAttribPointer(aTexCoordLoc, 2, GLES20.GL_FLOAT, false, 0, texCoords)
        GLES20.glEnableVertexAttribArray(aTexCoordLoc)

        GLES20.glDrawArrays(GLES20.GL_TRIANGLE_STRIP, 0, 4)

        /* ⚠️ 编码器那块必须**在 swap 之前**把相机的时间戳交给 EGL —— 编码器就拿它当
         * presentationTimeUs。丢了它（或换成墙钟），音视频时间轴就对不上，服务端会
         * 因为「音频跑到视频前面」整段丢 GOP（见 MicEncoder 顶部的说明）。 */
        if (ptsNs != 0L) EGLExt.eglPresentationTimeANDROID(eglDisplay, window, ptsNs)
        EGL14.eglSwapBuffers(eglDisplay, window)
    }

    private fun setPreviewSurfaceOnGl(s: Surface?) {
        if (eglDisplay == EGL14.EGL_NO_DISPLAY) return
        if (previewWindow != EGL14.EGL_NO_SURFACE) {
            EGL14.eglMakeCurrent(eglDisplay, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_SURFACE, eglContext)
            EGL14.eglDestroySurface(eglDisplay, previewWindow)
            previewWindow = EGL14.EGL_NO_SURFACE
        }
        if (s == null) { onLog("gl rotator: preview detached"); return }
        previewWindow = EGL14.eglCreateWindowSurface(eglDisplay, eglConfig, s,
            intArrayOf(EGL14.EGL_NONE), 0)
        if (previewWindow == EGL14.EGL_NO_SURFACE) {
            Log.e(TAG, "eglCreateWindowSurface(preview) failed: 0x" +
                Integer.toHexString(EGL14.eglGetError()))
            onLog("gl rotator: preview attach FAILED")
        } else {
            onLog("gl rotator: preview attached")
        }
    }

    private fun releaseGl() {
        try {
            /* 显式标类型：这个 Java 方法有单参/双参两个重载，裸 null 的推断
             * 不值得赌，写清楚了谁都一眼看懂。 */
            surfaceTexture?.setOnFrameAvailableListener(null as SurfaceTexture.OnFrameAvailableListener?)
            surfaceTexture?.release()
            inputSurface?.release()
            if (encoderWindow != EGL14.EGL_NO_SURFACE)
                EGL14.eglDestroySurface(eglDisplay, encoderWindow)
            if (previewWindow != EGL14.EGL_NO_SURFACE)
                EGL14.eglDestroySurface(eglDisplay, previewWindow)
            if (program != 0) GLES20.glDeleteProgram(program)
            if (textureId != 0) GLES20.glDeleteTextures(1, intArrayOf(textureId), 0)
            if (eglDisplay != EGL14.EGL_NO_DISPLAY) {
                EGL14.eglMakeCurrent(eglDisplay, EGL14.EGL_NO_SURFACE,
                    EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_CONTEXT)
                EGL14.eglDestroyContext(eglDisplay, eglContext)
                EGL14.eglTerminate(eglDisplay)
            }
        } catch (t: Throwable) {
            Log.w(TAG, "releaseGl", t)
        } finally {
            eglDisplay = EGL14.EGL_NO_DISPLAY
            encoderWindow = EGL14.EGL_NO_SURFACE
            previewWindow = EGL14.EGL_NO_SURFACE
            surfaceTexture = null
            program = 0
            textureId = 0
        }
    }

    private fun checkEgl(what: String) {
        val e = EGL14.eglGetError()
        if (e != EGL14.EGL_SUCCESS) throw IllegalStateException("$what failed: 0x${Integer.toHexString(e)}")
    }

    private fun buildProgram(): Int {
        val vs = """
            attribute vec4 aPosition;
            attribute vec4 aTexCoord;
            uniform mat4 uSTMatrix;
            varying vec2 vTexCoord;
            void main() {
                gl_Position = aPosition;
                vTexCoord = (uSTMatrix * aTexCoord).xy;
            }
        """.trimIndent()
        /* samplerExternalOES：相机进的是 OES 纹理，普通 sampler2D 取不到。 */
        val fs = """
            #extension GL_OES_EGL_image_external : require
            precision mediump float;
            varying vec2 vTexCoord;
            uniform samplerExternalOES uTexture;
            void main() { gl_FragColor = texture2D(uTexture, vTexCoord); }
        """.trimIndent()
        val v = compile(GLES20.GL_VERTEX_SHADER, vs)
        val f = compile(GLES20.GL_FRAGMENT_SHADER, fs)
        val p = GLES20.glCreateProgram()
        GLES20.glAttachShader(p, v); GLES20.glAttachShader(p, f)
        GLES20.glLinkProgram(p)
        val ok = IntArray(1)
        GLES20.glGetProgramiv(p, GLES20.GL_LINK_STATUS, ok, 0)
        if (ok[0] == 0) throw IllegalStateException("link failed: " + GLES20.glGetProgramInfoLog(p))
        GLES20.glDeleteShader(v); GLES20.glDeleteShader(f)
        return p
    }

    private fun compile(type: Int, src: String): Int {
        val s = GLES20.glCreateShader(type)
        GLES20.glShaderSource(s, src)
        GLES20.glCompileShader(s)
        val ok = IntArray(1)
        GLES20.glGetShaderiv(s, GLES20.GL_COMPILE_STATUS, ok, 0)
        if (ok[0] == 0) throw IllegalStateException(
            "shader compile failed (type=$type): " + GLES20.glGetShaderInfoLog(s) +
            /* 空的 info log 基本只有一个原因：编译时**没有当前 EGL 上下文**。
             * 把这条提示带上，省得下次又对着一句空错误发懵。 */
            "  [empty log usually means no current EGL context]")
        return s
    }

    private val texCoords: FloatBuffer = ByteBuffer
        .allocateDirect(8 * 4).order(ByteOrder.nativeOrder()).asFloatBuffer()
        .apply { put(floatArrayOf(0f, 1f, 1f, 1f, 0f, 0f, 1f, 0f)); position(0) }
}
