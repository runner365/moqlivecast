package com.example.moqlitepush

import android.annotation.SuppressLint
import android.content.Context
import android.hardware.camera2.CameraCaptureSession
import android.hardware.camera2.CameraCharacteristics
import android.hardware.camera2.CameraDevice
import android.hardware.camera2.CameraManager
import android.hardware.camera2.CaptureRequest
import android.media.MediaCodec
import android.media.MediaCodecInfo
import android.media.MediaFormat
import android.os.Handler
import android.os.HandlerThread
import android.os.SystemClock
import android.util.Log
import android.view.Surface
import android.view.WindowManager
import java.nio.ByteBuffer

/**
 * 摄像头 → H.264。
 *
 * ⚠️ **这一层里没有一行「YUV → H.264」**：路径是
 *
 *      Camera2 ──Surface──> GL 转正 ──Surface──> MediaCodec(createInputSurface) ──Annex-B──> JNI
 *
 * 中间那层 GL 只做**方向**（相机不会替你摆正传感器，见 CameraGlRotator），
 * 像素始终活在 GPU 上：YUV 既不经过 Kotlin、也不过 JNI，颜色转换与压缩都在
 * 硬件编码器里做。自己写 YUV→H.264（哪怕是调 libx264）会慢得多、代码多得多，
 * 而且**更差** —— 白白丢掉硬件编码器，还要每帧多拷一次过 JNI 边界。
 *
 * 唯一要自己动手的是 **Annex-B → AVCC**（MediaCodec 吐 Annex-B，moq-lite 要
 * AVCC），那在 C 侧做，见 annexb_avcc.c。
 */
class CameraEncoder(
    private val context: Context,
    private val onLog: (String) -> Unit,
) {

    companion object {
        private const val TAG = "MoqCamera"

        /* 640x480 @30fps，baseline —— 这是**目标**尺寸，实际用哪个见 pickSize()：
         * 相机会不支持某个尺寸，那时就近取一个能用的，并把选中的那个打出来。
         *
         * 640x480 = 4:3。⚠️ 和 web 播放端不一致：那边容器是 `aspect-ratio: 16/9`
         * （MoqPullView.vue），4:3 的流进去会**左右留黑边**（canvas 有
         * `object-fit: contain`）。要铺满就把这里改成 640x360（16:9）——
         * 但实测这台设备的前摄**没有** 640x360，改回去只会落到 1280x720。 */
        const val WIDTH = 640
        const val HEIGHT = 480
        const val FPS = 30
        const val BITRATE = 1_200_000

        /** H.264 profile。baseline(66) 没有 B 帧、没有 CABAC —— 兼容性最好，
         *  老播放器/浏览器也都能解。 */
        private val PROFILE = MediaCodecInfo.CodecProfileLevel.AVCProfileBaseline

        /** 关键帧间隔（秒）。调小一点：对端新加入时等得更短。 */
        private const val IFRAME_INTERVAL_S = 2

        /** 接输出用的复用缓冲。关键帧实测几十 KB，留足余量。 */
        private const val OUT_BUF = 512 * 1024

        /** 本轮**编码栅格**的宽高比（w/h）。⚠️ 别拿它去摆预览框 —— 预览框是固定的
         *  竖的（见 [PREVIEW_ASPECT]），而这个是码流的比例，会随旋转/相机回退变化。
         *  留着是为了日志和排错（0 = 还没定）。 */
        @Volatile
        var currentAspect: Float = 0f
            private set

        /** 预览视图的宽高比 —— **竖的 3:4**，和推流/画面比例**无关**。
         *
         *  ⚠️ 为什么钉成竖的：手机是竖着拿的，而 4:3 的横向画面铺满一整个竖框
         *  本来就得二选一 —— 留黑边（contain）或者裁掉两侧（cover）。这里选 cover
         *  （见 CameraGlRotator.drawTo 的 cover 分支）：**画面不变形**，代价是看不到
         *  左右两侧。UI 上有个 `aspect:` 按钮可以当场换档（4:3 / 1:1 / 3:4 / 2:3 /
         *  9:16 / 9:19.5 / 9:20），这里只是它的初始值 —— 定在 3:4，
         *  即 4:3 传感器竖屏取景的常规裁法（保留 56% 的画幅宽度）。
         *  ⚠️ 它**不**等于编码宽高比，也不是「转正后画面的比例」—— 那两者都会随
         *  旋转变化，而预览框的形状不该跟着变。 */
        const val PREVIEW_ASPECT = 0.75f

    }

    private var codec: MediaCodec? = null
    private var inputSurface: Surface? = null
    private var camera: CameraDevice? = null
    private var session: CameraCaptureSession? = null
    /** 预览输出（可选）。由 Activity 的 SurfaceView 提供。 */
    private var previewSurface: Surface? = null

    /* 本轮的摄像头与**实际**编码尺寸。
     * ⚠️ 实际尺寸不一定等于 WIDTH×HEIGHT —— 相机会不支持某个尺寸（见 pickSize），
     * 编码器必须和 capture session 的输出尺寸**完全一致**，所以这两者要一起定。 */
    private var cameraId: String? = null
    /** 相机输出（OES 纹理）的尺寸 —— 相机支持的横向尺寸，如 640x480。 */
    private var camW = WIDTH
    private var camH = HEIGHT
    /** **编码栅格**尺寸。转 90/270 时它是 camW/camH 换过宽高的竖构图（480x640）——
     *  转正后的画面本来就是竖的，编进横向栅格等于把它横向拉满。见 CameraGlRotator.rotationFor。 */
    private var encW = WIDTH
    private var encH = HEIGHT
    private var encBitrate = BITRATE
    /** 相机帧的转正层。相机的输出挂在它身上，编码器和预览都由它来画。 */
    private var gl: CameraGlRotator? = null

    private var cameraThread: HandlerThread? = null
    private var cameraHandler: Handler? = null
    private var codecThread: HandlerThread? = null
    private var codecHandler: Handler? = null

    private val outBuf = ByteArray(OUT_BUF)
    @Volatile private var running = false

    private fun log(s: String) {
        Log.i(TAG, s)
        onLog(s)
    }

    /** 预览 Surface 的来去都走这里 —— **GL 才是预览的绘制者**，它得知道。 */
    fun setPreviewSurface(s: Surface?) {
        previewSurface = s
        gl?.setPreviewSurface(s)
    }

    /** 打开摄像头并开始编码。调用方必须先拿到 CAMERA 权限。
     *
     * [preview] 是可选的**预览输出**（SurfaceView 的 Surface）。给不给都行：
     * 编码器的输入 Surface 是 createInputSurface() 自己建的，不依赖它。
     * 预览只是往**同一条 capture session** 上多挂一个输出 —— 所以
     * ⚠️ **预览 Surface 必须比 session 活得久**：Activity 被销毁后它的 Surface
     * 会失效，那条 session 会跟着报错，采集一起停。这里不自动重建（够用即可），
     * 真要长期后台推流得在 surfaceDestroyed 时重建 session。 */
    @SuppressLint("MissingPermission")
    fun start(preview: Surface? = null) {
        if (running) return
        running = true
        previewSurface = preview

        cameraThread = HandlerThread("moq-cam").also { it.start() }
        cameraHandler = Handler(cameraThread!!.looper)
        codecThread = HandlerThread("moq-venc").also { it.start() }
        codecHandler = Handler(codecThread!!.looper)

        // ⚠️ 摄像头与**尺寸**必须在建编码器**之前**定下来：编码器的输入 Surface
        // 是按尺寸建的，capture session 又要求输出尺寸与它逐字相等。
        val mgr = context.getSystemService(Context.CAMERA_SERVICE) as CameraManager
        val id = pickCamera(mgr)
        if (id == null) {
            log("no camera available")
            running = false
            return
        }
        cameraId = id
        logOrientation(mgr, id)
        val sz = pickSize(mgr, id)
        camW = sz.first
        camH = sz.second

        /* ⚠️ 旋转要**在建编码器之前**定下来 —— 它决定编码栅格是横的还是竖的。
         * 相机只会给横向尺寸，而转正后的画面是竖的，所以这里把宽高换过来；
         * 真正把内容转过去的是 GL 那层（同一个角度）。 */
        val ch = mgr.getCameraCharacteristics(id)
        val sensorOrient = ch.get(CameraCharacteristics.SENSOR_ORIENTATION) ?: 0
        val isFront = ch.get(CameraCharacteristics.LENS_FACING) ==
                      CameraCharacteristics.LENS_FACING_FRONT
        val (rotDeg, mirror) = CameraGlRotator.rotationFor(sensorOrient, displayDegrees(), isFront)
        val swap = rotDeg % 180 == 90
        encW = if (swap) camH else camW
        encH = if (swap) camW else camH
        currentAspect = if (encH > 0) encW.toFloat() / encH else 0f
        /* ⚠️ 码率跟**实际**尺寸走，不是跟着 WIDTH×HEIGHT 走。相机给不出目标尺寸时
         * （实测前摄没有 640x360，就近取到了 **1280x720**），面积是目标的 4 倍 ——
         * 还按固定的 1.2 Mbps 编就是饿着编：720p@1.2Mbps 肉眼可见地糊。按面积等比
         * 放大，码率/像素数保持恒定，画质才不随尺寸漂。 */
        val ratio = camW.toLong() * camH / (WIDTH.toLong() * HEIGHT)
        encBitrate = (BITRATE.toLong() * ratio).toInt().coerceIn(400_000, 8_000_000)
        if (camW != WIDTH || camH != HEIGHT) {
            log("camera $id does not support ${WIDTH}x${HEIGHT}; using ${camW}x${camH} " +
                "@${encBitrate / 1000}kbps")
        }
        if (swap) log("frame is rotated ${rotDeg}° → encode raster becomes ${encW}x${encH}")

        startCodec()

        /* 相机帧先过 GL 转正，再进编码器（见 CameraGlRotator 顶部）。 */
        val encSurface = inputSurface
        if (encSurface == null) {
            log("no encoder input surface — cannot start the rotator")
            running = false
            return
        }
        gl = CameraGlRotator { log(it) }
        gl?.start(encSurface, camW, camH, rotDeg, mirror)
        gl?.setPreviewSurface(previewSurface)

        openCamera(id)
    }

    /** 当前显示方向（0/90/180/270），转正公式要用。 */
    @Suppress("DEPRECATION")
    private fun displayDegrees(): Int = when (
        (context.getSystemService(Context.WINDOW_SERVICE) as WindowManager).defaultDisplay.rotation
    ) {
        Surface.ROTATION_90 -> 90
        Surface.ROTATION_180 -> 180
        Surface.ROTATION_270 -> 270
        else -> 0
    }

    private fun startCodec() {
        val fmt = MediaFormat.createVideoFormat("video/avc", encW, encH).apply {
            // ⚠️ 必须是 COLOR_FormatSurface —— 这就是「让相机直接写进编码器」的开关。
            // 换成 COLOR_FormatYUV420Flexible 就变成 ByteBuffer 输入模式，那时才需要
            // 你自己喂 YUV（也正是我们不想要的）。
            setInteger(MediaFormat.KEY_COLOR_FORMAT,
                MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface)
            setInteger(MediaFormat.KEY_BIT_RATE, encBitrate)
            setInteger(MediaFormat.KEY_FRAME_RATE, FPS)
            setInteger(MediaFormat.KEY_I_FRAME_INTERVAL, IFRAME_INTERVAL_S)
            /* profile：要 baseline。只是个**请求** —— 设备不认就会忽略或直接
             * 抛异常（configure 那里会记下来），而真正生效的值看 avcC 的第二个
             * 字节（codec 串 avc1.PPCCLL 里的 PP），日志里有。 */
            setInteger(MediaFormat.KEY_PROFILE, PROFILE)
        }

        val c = MediaCodec.createEncoderByType("video/avc")
        c.configure(fmt, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE)
        inputSurface = c.createInputSurface()
        c.setCallback(object : MediaCodec.Callback() {
            override fun onInputBufferAvailable(mc: MediaCodec, index: Int) {
                // Surface 输入模式下没有输入 ByteBuffer —— 这是正常的，不是漏了什么。
            }

            override fun onOutputBufferAvailable(mc: MediaCodec, index: Int,
                                                 info: MediaCodec.BufferInfo) {
                try {
                    handleOutput(mc, index, info)
                } catch (t: Throwable) {
                    Log.e(TAG, "handleOutput failed", t)
                }
            }

            override fun onError(mc: MediaCodec, e: MediaCodec.CodecException) {
                log("video codec error: ${e.diagnosticInfo}")
            }

            override fun onOutputFormatChanged(mc: MediaCodec, format: MediaFormat) {
                publishVideoConfig(format)
            }
        }, codecHandler)

        c.start()
        codec = c
        // ⚠️ `$encWx` 会被当成一个叫「encWx」的变量（简单名会一路吃字母数字），
        // 要拼接就得用 `${encW}x` 这种带花括号的写法。
        log("video encoder started: ${encW}x${encH} @${encBitrate / 1000}kbps, ${FPS}fps")
    }

    /**
     * csd-0/csd-1 到齐就拼 avcC 交给 native —— 这是 catalog 里那条 rendition 的
     * description，**没有它广播就没有 catalog**，对端既不被公告也解不开。
     */
    private fun publishVideoConfig(format: MediaFormat) {
        try {
            /* 先交代清楚「我们请求的」「编码器实际编的」「裁完可见的」这三个尺寸。
             * ⚠️ 它们**不一定相等**：MediaCodec 常按 16 的倍数向上取整（H.264 的
             * 对齐要求），也可能带 crop。播放端按码流（SPS）解、按 catalog 里我们
             * 声明的数字显示，两者一旦不一致，画面就会**错位** —— 而不是被旋转。
             * 排查「画面是歪的」时，先把这三个数摆出来，能立刻把「错位」和
             * 「真的转了」分开（后者看 logOrientation 打的 sensor 角度）。 */
            fun intOr(key: String, dflt: Int) =
                if (format.containsKey(key)) format.getInteger(key) else dflt
            val outW = intOr(MediaFormat.KEY_WIDTH, -1)
            val outH = intOr(MediaFormat.KEY_HEIGHT, -1)
            val cl = intOr("crop-left", 0)
            val cr = intOr("crop-right", outW - 1)
            val ct = intOr("crop-top", 0)
            val cb = intOr("crop-bottom", outH - 1)
            log("video format: requested=${encW}x${encH} encoder=${outW}x${outH} " +
                "visible=${cr - cl + 1}x${cb - ct + 1} (crop=$cl,$ct-$cr,$cb)")

            val csd0 = format.getByteBuffer("csd-0")
            val csd1 = format.getByteBuffer("csd-1")
            val a0 = csd0?.let { ByteBuffer.allocate(it.remaining()).also { b -> b.put(it).flip() }.array() }
            val a1 = csd1?.let { ByteBuffer.allocate(it.remaining()).also { b -> b.put(it).flip() }.array() }
            if (a0 == null) {
                log("video format changed but csd-0 missing")
                return
            }
            val avcc = MoqNative.nativeBuildAvcC(a0, a1)
            if (avcc == null) {
                log("build avcC failed (SPS/PPS not found in csd)")
                return
            }
            val rc = MoqNative.nativeVideoConfig(avcc, encW, encH, "0.h264")
            /* codec 串是从 avcC 现拼的，所以这一行能直接看出 profile 到底是不是
             * baseline —— avc1.42xxxx = baseline(0x42)，avc1.64xxxx = high(0x64)。 */
            val prof = if (avcc.size >= 2) String.format("%02x", avcc[1]) else "??"
            log("video config: avcC ${avcc.size} B (profile=${prof}) -> native rc=$rc")
        } catch (t: Throwable) {
            Log.e(TAG, "publishVideoConfig failed", t)
        }
    }

    private fun handleOutput(mc: MediaCodec, index: Int, info: MediaCodec.BufferInfo) {
        if (info.size > 0) {
            val buf = mc.getOutputBuffer(index)
            if (buf != null) {
                val n = minOf(info.size, outBuf.size)
                buf.position(info.offset)
                buf.limit(info.offset + n)
                buf.get(outBuf, 0, n)

                val key = (info.flags and MediaCodec.BUFFER_FLAG_KEY_FRAME) != 0

                // ⚠️ 必须在 releaseOutputBuffer **之前**交给 native：buffer 还回去
                // 之后这块内存就不归我们了，里面的字节随时可能被编解码器重用。
                // native 侧会拷一份入队，所以返回之后 release 是安全的。
                MoqNative.nativeSendVideo(info.presentationTimeUs, outBuf, n, key)
            }
        }
        // 第二个参数 false = **不要把 buffer 渲染到 Surface**。我们不是播它，
        // 传 true 会在没配 output Surface 的设备上抛异常。
        try {
            mc.releaseOutputBuffer(index, false)
        } catch (t: Throwable) {
            Log.w(TAG, "releaseOutputBuffer failed", t)
        }
    }

    @SuppressLint("MissingPermission")
    private fun openCamera(id: String) {
        val mgr = context.getSystemService(Context.CAMERA_SERVICE) as CameraManager
        val facing = mgr.getCameraCharacteristics(id)
            .get(CameraCharacteristics.LENS_FACING)
        val which = when (facing) {
            CameraCharacteristics.LENS_FACING_FRONT -> "front"
            CameraCharacteristics.LENS_FACING_BACK -> "back"
            else -> "other"
        }

        mgr.openCamera(id, object : CameraDevice.StateCallback() {
            override fun onOpened(dev: CameraDevice) {
                camera = dev
                createSession(dev)
            }

            override fun onDisconnected(dev: CameraDevice) {
                log("camera disconnected")
                dev.close()
                if (camera === dev) camera = null
            }

            override fun onError(dev: CameraDevice, error: Int) {
                log("camera error: $error")
                dev.close()
                if (camera === dev) camera = null
            }
        }, cameraHandler)
        log("camera opened: id=$id ($which)")
    }

    /** 选摄像头：**前摄优先**，没有前摄再退后摄，都没有就用列表里第一个。
     *  推流一般对着人，前摄是更常见的默认。 */
    private fun pickCamera(mgr: CameraManager): String? {
        val ids = mgr.cameraIdList
        fun firstFacing(facing: Int) = ids.firstOrNull { id ->
            mgr.getCameraCharacteristics(id).get(CameraCharacteristics.LENS_FACING) == facing
        }
        return firstFacing(CameraCharacteristics.LENS_FACING_FRONT)
            ?: firstFacing(CameraCharacteristics.LENS_FACING_BACK)
            ?: ids.firstOrNull()
    }

    /** 在相机支持的输出尺寸里**就近**取一个给编码器用。
     *
     * ⚠️ 为什么不能直接用 WIDTH×HEIGHT：capture session 要求「输出 Surface 的尺寸
     * 必须是相机支持的尺寸之一」，不支持的话 `createCaptureSession` 直接抛
     * IllegalArgumentException —— 整条 session 建不起来，表现是**一路黑屏、
     * 一帧视频都没有**，而日志里只有一句 createCaptureSession failed。
     * 640x480 虽然通用，但**不保证**每台设备的前摄都有（有些只给 1:1 和 16:9），
     * 所以这里挑一个最接近的：完全匹配就用它，否则按下面的对数比取最近的。
     * 支持的尺寸会整行打进日志（`supported sizes: ...`），选用哪个也打。 */
    private fun pickSize(mgr: CameraManager, id: String): Pair<Int, Int> {
        val sizes = try {
            mgr.getCameraCharacteristics(id)
                .get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP)
                ?.getOutputSizes(MediaCodec::class.java)
        } catch (t: Throwable) {
            Log.w(TAG, "getOutputSizes failed", t)
            null
        }
        if (sizes.isNullOrEmpty()) {
            log("camera $id reports no output sizes; falling back to ${WIDTH}x${HEIGHT}")
            return WIDTH to HEIGHT
        }

        /* 先把相机支持的尺寸**全列出来**。选不中想要的尺寸时第一件想知道的就是
         * 「它到底有哪几个」—— 打成一整行，adb logcat 里一眼看全。 */
        log("camera $id supported sizes: " +
            sizes.joinToString(" ") { "${it.width}x${it.height}" })

        sizes.firstOrNull { it.width == WIDTH && it.height == HEIGHT }?.let {
            return it.width to it.height
        }

        /* 没有完全匹配的就取**最接近 640x480** 的那个。
         *
         * 判据用**对数比**，形状与面积各占一半权重，而且与尺度无关
         * （320x240 和 2560x1920 相对 640x480 的「远」是一样的）。
         *
         * ⚠️ 不要写成 `面积差 + 形状差 × 常数` 那种形式：两个量量纲不同，
         * 常数怎么取都会在某个区间失真 —— 上一版就是这么挑出方图（480x480）来的：
         * 它面积上离目标很近，形状那一项被淹没，于是画面成了 1:1，
         * 而 web 播放端的 canvas 直接按解码帧的尺寸走（videoRenderer 的
         * `canvas resized`），两边一起变成正方形。用比值就没有这个量纲问题。 */
        val wantArea = WIDTH.toDouble() * HEIGHT
        val wantAspect = WIDTH.toDouble() / HEIGHT
        val best = sizes.minByOrNull { s ->
            val aspectErr = kotlin.math.ln(s.width.toDouble() / s.height / wantAspect)
            val areaErr = kotlin.math.ln(s.width.toDouble() * s.height / wantArea)
            kotlin.math.abs(aspectErr) + kotlin.math.abs(areaErr)
        }
        return (best?.width ?: WIDTH) to (best?.height ?: HEIGHT)
    }

    /** 把方向这件事的**输入**打出来：传感器装在哪、当前显示方向、前摄与否。
     *  ⚠️ 这里**不算**该转多少度 —— 那个决定在 CameraGlRotator（它把公式、镜像
     *  开关和兜底微调放在一起，见那里的 ROTATION_ADJUST_DEG）。两处各算一遍
     *  只会得到两个互相矛盾的数字。 */
    @Suppress("DEPRECATION")
    private fun logOrientation(mgr: CameraManager, id: String) {
        try {
            val ch = mgr.getCameraCharacteristics(id)
            val sensor = ch.get(CameraCharacteristics.SENSOR_ORIENTATION) ?: 0
            val facing = ch.get(CameraCharacteristics.LENS_FACING)
            val isFront = facing == CameraCharacteristics.LENS_FACING_FRONT
            val wm = context.getSystemService(Context.WINDOW_SERVICE) as WindowManager
            val displayDeg = when (wm.defaultDisplay.rotation) {
                Surface.ROTATION_90 -> 90
                Surface.ROTATION_180 -> 180
                Surface.ROTATION_270 -> 270
                else -> 0
            }
            log("camera $id orientation: sensor=$sensor display=$displayDeg " +
                "facing=${if (isFront) "front" else "back"} " +
                "(rotation decision is logged by the gl rotator)")
        } catch (t: Throwable) {
            Log.w(TAG, "logOrientation failed", t)
        }
    }

    private fun createSession(dev: CameraDevice) {
        /* ⚠️ 相机现在**只有一个**输出：GL 的输入 Surface（相机帧先过它转正）。
         * 预览不再挂在相机上 —— 它是 GL 画的第二个靶子，所以
         *   - 不再受「输出尺寸必须是相机支持的尺寸」约束（那正是以前的坑）；
         *   - 相机不会为了预览多出一路流。
         * 顺带：尺寸也不必再和 view 对齐，GL 那边按转正后的比例做信箱式留边。 */
        /* ⚠️ **必须等 GL 层把 Surface 建出来**：相机唯一的输出就是它，而 GL 是异步
         * 起来的 —— 实测相机 320ms 就 open 好了，GL 要 635ms，不等的话这里读到
         * null，session 建不起来，表现是「预览和视频一起消失」。
         * 本函数跑在 cameraHandler（不是主线程），等得起。 */
        val rotator = gl
        if (rotator == null || !rotator.awaitReady(3000)) {
            /* 多半是 initGl 失败，见 MoqGl 那几行。相机没别的输出，所以**不会**有
             * 预览也不会有视频 —— 直说，否则表现就是「画面凭空消失」。 */
            log("rotator not ready → no camera output (preview and video will be MISSING; see MoqGl errors above)")
            return
        }
        val camTarget = rotator.inputSurface ?: run {
            log("rotator reported ready but has no input surface — giving up")
            return
        }

        val targets = ArrayList<Surface>(1)
        targets.add(camTarget)
        log("capture session outputs: gl-input (rotator) ")

        try {
            @Suppress("DEPRECATION")
            dev.createCaptureSession(targets, object : CameraCaptureSession.StateCallback() {
                override fun onConfigured(s: CameraCaptureSession) {
                    if (!running) return
                    session = s
                    try {
                        val req = dev.createCaptureRequest(CameraDevice.TEMPLATE_RECORD).apply {
                            for (t in targets) addTarget(t)   // 每个输出都要显式 addTarget
                            // 自动曝光/对焦——TEMPLATE_RECORD 默认就是，这里显式写出来
                            // 是为了让「我们依赖默认值」这件事在代码里看得见。
                            set(CaptureRequest.CONTROL_MODE, CaptureRequest.CONTROL_MODE_AUTO)
                        }.build()
                        s.setRepeatingRequest(req, null, cameraHandler)
                        log("capture session running (${targets.size} output(s))")
                    } catch (t: Throwable) {
                        log("setRepeatingRequest failed: ${t.message}")
                    }
                }

                override fun onConfigureFailed(s: CameraCaptureSession) {
                    log("capture session configuration FAILED")
                }
            }, cameraHandler)
        } catch (t: Throwable) {
            log("createCaptureSession failed: ${t.message}")
        }
    }

    /** ⚠️ 只能在**非主线程**上调（PushService 有一条专门的拆流线程）：
     *  下面每一步都可能阻塞，跑在主线程上就是 ANR。 */
    fun stop() {
        running = false
        val t0 = SystemClock.elapsedRealtime()
        fun step(what: String) =
            log("stop: $what +${SystemClock.elapsedRealtime() - t0}ms")

        /* ⚠️ 顺序要紧：**先让相机停止往编码器的输入 Surface 里写**，再停编码器。
         * MediaCodec.stop() 对 Surface 输入的编码器要等生产者把它手上的 buffer
         * 交回来；相机还在出帧的时候调它就可能一直等下去。所以先
         * stopRepeating + abortCaptures（同步生效），再 close session、关相机，
         * 最后才动 codec。
         * 注意 session/camera 的 close 是**异步**的（落在 cameraHandler 上），
         * 真正断流发生在稍后，但 stopRepeating/abortCaptures 已经把重复请求
         * 撤掉了，编码器不会再收到新的输入帧。 */
        try { session?.stopRepeating() } catch (_: Throwable) {}
        try { session?.abortCaptures() } catch (_: Throwable) {}
        try { session?.close() } catch (_: Throwable) {}
        session = null
        try { camera?.close() } catch (_: Throwable) {}
        camera = null
        step("camera closed")

        /* ⚠️ GL 那层必须**先于**编码器的 inputSurface 释放：它在上面挂了一个
         * EGL window surface，并持有 SurfaceTexture。反过来就是 use-after-free。 */
        try { gl?.stop() } catch (_: Throwable) {}
        gl = null
        step("gl rotator stopped")

        try { codec?.stop() } catch (_: Throwable) {}
        try { codec?.release() } catch (_: Throwable) {}
        codec = null
        try { inputSurface?.release() } catch (_: Throwable) {}
        inputSurface = null
        step("codec released")

        cameraThread?.quitSafely(); cameraThread = null; cameraHandler = null
        codecThread?.quitSafely(); codecThread = null; codecHandler = null
        step("threads quit")
    }
}
