package com.example.moqlitepush

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.os.Build
import android.os.IBinder
import android.os.SystemClock
import android.util.Log
import java.util.concurrent.Executors
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicBoolean

/**
 * 推流的前台服务：持有两路编码器，并驱动 native 侧的推流。
 *
 * 为什么要前台服务：采集摄像头/麦克风在后台会被系统掐掉，而 Android 14(API 34)
 * 起还要求声明 **foregroundServiceType** 并持有对应的
 * `FOREGROUND_SERVICE_CAMERA` / `FOREGROUND_SERVICE_MICROPHONE` —— 见 manifest。
 * 少了任何一个，启动时会直接抛 SecurityException / MissingForegroundServiceTypeException。
 *
 * 启动顺序是有讲究的：
 *   ① nativeStart —— 先连（连上后它自己 publish；轨要等编解码参数到齐）
 *   ② 再开编码器 —— 编码器一跑就会吐 csd/帧，native 侧收到 csd 才 publish_track
 * 反过来的话，编码器吐的前一批帧会因为「轨还没声明」被丢掉（native 侧会计进
 * dropped，不会报错）。
 */
class PushService : Service() {

    companion object {
        private const val TAG = "MoqPushSvc"

        const val ACTION_START = "com.example.moqlitepush.action.START"
        const val ACTION_STOP  = "com.example.moqlitepush.action.STOP"
        const val ACTION_RESTART = "com.example.moqlitepush.action.RESTART"
        const val EXTRA_URL       = "url"
        const val EXTRA_BROADCAST = "broadcast"

        private const val CHANNEL_ID = "moqpush"
        private const val NOTIF_ID   = 1

        /** 连续掉线最多自动重连几次。超过就不折腾了，停下来把人叫醒。 */
        private const val MAX_AUTO_RESTARTS = 10

        /** 手机会用局域网的 IP 拨服务端：`/etc/hosts` 里那条域名→127.0.0.1 只在
         *  开发机上有效，手机上解析不到。能用 IP 是因为本库不校验证书。 */
        const val DEFAULT_URL = "moql://192.168.1.143:4433/anon"
        const val DEFAULT_BROADCAST = "android.hang"

        @Volatile var running = false
            private set
        @Volatile var lastLog = ""
            private set

        /** 预览用的 Surface，由 Activity 的 SurfaceView 在 surfaceCreated 时放进来。
         * ⚠️ 跨 Activity/Service 传 Surface 只能这样传（Surface 不是 Parcelable）。
         * 它只在 Activity 存活期间有效 —— 见 CameraEncoder.start() 的说明。
         *
         * ⚠️ **private，而且只能通过 [setPreviewSurface] 改**，两个原因：
         *   ① 公开的 var 会生成一个 `setPreviewSurface(Surface)` 的 JVM setter，
         *      与下面那个同名函数**签名撞车**（Platform declaration clash）；
         *      private 属性在类内是直接读写字段的，不生成 setter，也就没这回事。
         *   ② 光记下这块 Surface 不够 —— GL 层才是预览的绘制者，它得同步知道。 */
        @Volatile private var previewSurface: android.view.Surface? = null

        /** 当前活着的那个实例（同一时刻只会有一个）。给 companion 里的
         *  [setPreviewSurface] 传话用。 */
        @Volatile private var current: PushService? = null

        fun start(ctx: Context, url: String, broadcast: String) {
            val i = Intent(ctx, PushService::class.java).apply {
                action = ACTION_START
                putExtra(EXTRA_URL, url)
                putExtra(EXTRA_BROADCAST, broadcast)
            }
            ctx.startForegroundService(i)
        }

        fun stop(ctx: Context) {
            ctx.startService(Intent(ctx, PushService::class.java).apply { action = ACTION_STOP })
        }

        /** 用**同一组参数**重新起一遍。改完采集相关的参数（比如画面方向）后调它 ——
         *  那些参数要重建编码器才生效，光改不重起是没用的。 */
        fun restart(ctx: Context) {
            ctx.startService(Intent(ctx, PushService::class.java).apply { action = ACTION_RESTART })
        }

        /** 预览 Surface 的来去都走这里。**不要**直接写 [previewSurface]：
         *  预览现在是由 GL 层画的（见 CameraGlRotator），它得跟着知道。 */
        fun setPreviewSurface(s: android.view.Surface?) {
            previewSurface = s
            current?.camera?.setPreviewSurface(s)
        }
    }

    private var camera: CameraEncoder? = null
    /* 这次启动用的参数，restart() 要拿它重起一遍。 */
    private var lastUrl = DEFAULT_URL
    private var lastBc = DEFAULT_BROADCAST
    private var mic: MicEncoder? = null

    /* 拆流用的工作线程。
     *
     * ⚠️ **拆流不能跑在主线程上** —— 这是「点 Stop 界面冻住」的原因。
     * `onStartCommand` 默认在**主线程**上跑，而 stopEverything() 里每一件事
     * 都可能阻塞：`MediaCodec.stop()/release()`（Surface 输入的编码器尤其，
     * 相机若还在往输入 Surface 里写就可能等很久）、`AudioRecord.release()`、
     * `CameraDevice.close()`，以及 native 的 `uv_thread_join()`。任意一个卡住
     * 就把主线程带走 —— 界面冻死、5 秒后 ANR。
     *
     * 挪到这条线程上之后，主线程只负责把请求交出去，立刻就能返回。
     * 这个线程既不是主线程、也不是 MediaCodec 的回调线程（`codecHandler`）：
     * 文档明确禁止在回调线程上调 stop()/release()，所以不能再借 `codecThread`。 */
    private val teardown = Executors.newSingleThreadExecutor { r ->
        Thread(r, "moq-teardown").apply { isDaemon = true }
    }

    /** 拆流只跑一次（Stop 可能被点两下，`onDestroy` 也会兜底调一次）。 */
    private val teardownStarted = AtomicBoolean(false)

    /* 会话掉线看门狗。
     *
     * ⚠️ 为什么必须有：native 的 loop 线程在会话关闭后**就退出了**，此后编解码器
     * 再吐帧也没人取（native 侧会直接丢掉）。但主线程这一侧**完全不知道** ——
     * `running` 还是 true、界面还是 RUNNING、采集还在跑。表现就是用户看到的那种
     * 「connected=false 却 published=true、dropped 一直涨」。
     *
     * 掉线的判据是「**曾经**连上过、现在 connected=false」：启动初期本来就还没连上，
     * 那时候 false 不算掉线。 */
    private val watchdog = Executors.newSingleThreadScheduledExecutor { r ->
        Thread(r, "moq-watchdog").apply { isDaemon = true }
    }
    private var everConnected = false
    private var downStreak = 0
    private var watchdogTask: java.util.concurrent.ScheduledFuture<*>? = null
    /** 连续自动重连的次数。稳定连满 30s 后清零（算的是「连续掉线」，不是「掉过几次」）。 */
    private var autoRestarts = 0
    private var upSinceMs = 0L

    private fun startWatchdog() {
        everConnected = false
        downStreak = 0
        /* ⚠️ 每次 start 都要换一条新的，所以先把旧的取消掉。不移除的话每启动一次
         * 就多一条常驻轮询 —— 以前只有 Start 还好，加了 restart（方向标定要点很多次）
         * 之后就会明显堆积。 */
        watchdogTask?.cancel(false)
        watchdogTask = watchdog.scheduleWithFixedDelay({
            try {
                if (running) {
                    val s = MoqNative.stats()
                    if (s.connected) {
                        everConnected = true
                        downStreak = 0
                        val now = SystemClock.elapsedRealtime()
                        if (upSinceMs == 0L) upSinceMs = now
                        else if (now - upSinceMs > 30_000) autoRestarts = 0
                    } else {
                        upSinceMs = 0L
                        if (everConnected && ++downStreak >= 4) {   /* ~2s 确认 */
                            notify("session down (connected=false for ~2s)")
                            /* 把断线前最后的原生日志倒进 Logcat。断线原因（比如
                             * QUIC 的 PTO retrans / no viable path）只出现在**库自己**
                             * 的日志里，平时只落文件 —— 不 dump 的话一次 adb logcat
                             * 什么都看不到，还得再去 pull 那个文件。
                             *
                             * ⚠️ 窗口要够大、而且**相邻重复行必须折叠**：拆流时
                             * 「udp send error: operation canceled」这类会成串刷屏，
                             * 上一版只 dump 30 行、又不折重，结果整整一屏全是它，
                             * 真正的原因（PTO retrans over / on_connection_dead）
                             * 反被挤出窗口 —— 那就白 dump 了。 */
                            val tail = NativeLog.tail(200)
                            var prev: String? = null
                            var run = 0
                            for (line in tail) {
                                if (line == prev) { run++; continue }
                                if (run > 0) Log.i(TAG, "native:   (previous line ×${run + 1})")
                                run = 0
                                prev = line
                                Log.i(TAG, "native: $line")
                            }
                            if (run > 0) Log.i(TAG, "native:   (previous line ×${run + 1})")

                            /* ⚠️ **不要直接停**。这条链路实测会自己恢复（停 9~13 秒后又通），
                             * 而「停掉」要等人手动按 Start。所以先自动重连。
                             * 但也不能无限重连 —— 连续掉线超过 MAX_AUTO_RESTARTS 次就认输，
                             * 停掉并把原因显示出来，否则用户看到的是「一直在重连」而不知道
                             * 出了什么事。计数在稳定连满 30s 后清零。 */
                            if (autoRestarts < MAX_AUTO_RESTARTS) {
                                autoRestarts++
                                notify("session down → auto-restart #$autoRestarts/$MAX_AUTO_RESTARTS")
                                restart()
                            } else {
                                notify("session down → giving up after $MAX_AUTO_RESTARTS restarts")
                                requestStop()
                            }
                        }
                    }
                }
            } catch (t: Throwable) {
                Log.w(TAG, "watchdog failed", t)
            }
        }, 1000, 500, TimeUnit.MILLISECONDS)
    }

    override fun onCreate() {
        super.onCreate()
        current = this
    }

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        when (intent?.action) {
            ACTION_RESTART -> {
                restart()
                return START_NOT_STICKY
            }
            ACTION_STOP -> {
                /* 交给工作线程，本函数立刻返回 —— 主线程不被拆流阻塞。 */
                requestStop()
                return START_NOT_STICKY
            }
            else -> {
                val url = intent?.getStringExtra(EXTRA_URL)?.takeIf { it.isNotBlank() } ?: DEFAULT_URL
                val bc  = intent?.getStringExtra(EXTRA_BROADCAST)?.takeIf { it.isNotBlank() } ?: DEFAULT_BROADCAST
                startForegroundCompat()
                startEverything(url, bc)
            }
        }
        // 不要 START_STICKY：系统重启这个服务时不会重新授权摄像头/麦克风，
        // 拉起来也是个哑的。
        return START_NOT_STICKY
    }

    private fun notify(s: String) {
        Log.i(TAG, s)
        lastLog = s
    }

    private fun startEverything(url: String, broadcast: String) {
        if (running) { notify("already running"); return }
        lastUrl = url
        lastBc = broadcast
        running = true
        teardownStarted.set(false)     /* 新一轮，允许再拆一次 */

        // 库自己的日志落文件 —— native 的 stdout 在 Android 上不可见，
        // 只能写文件再 `adb pull /data/data/<pkg>/files/moq_lite.log`。
        val logPath = "${filesDir.absolutePath}/moq_lite.log"
        /* 把路径报出来：它在应用私有目录里（/data/data/<pkg>/files/），
         * 既不能直接 adb pull，界面上也看不到 —— 不报的话想知道它在哪就只能翻代码。 */
        notify("library log: $logPath")

        val rc = MoqNative.nativeStart(url, broadcast, logPath)
        notify("nativeStart rc=$rc url=$url broadcast=$broadcast")
        if (rc != 0) {
            running = false
            return
        }

        val pv = previewSurface
        notify("preview surface: " + if (pv != null && pv.isValid) "present" else "none")

        camera = CameraEncoder(applicationContext) { notify(it) }.also { it.start(pv) }
        mic = MicEncoder(applicationContext) { notify(it) }.also { it.start() }

        startWatchdog()
    }

    /** 拆掉再起来（同一组参数）。放在**拆流线程**上顺序执行，因为拆和起都必须
     *  离开主线程，而且起必须在拆**之后** —— 直接连着调两个 startService 是
     *  拿不到这个顺序保证的。 */
    private fun restart() {
        if (!running) { startEverything(lastUrl, lastBc); return }
        notify("restarting…")
        teardown.execute {
            stopEverything()
            teardownStarted.set(false)      /* 允许下一轮再拆 */
            startEverything(lastUrl, lastBc)
        }
    }

    /** 请求拆流。幂等；把整段拆流丢给工作线程，调用方（主线程）立刻返回。 */
    private fun requestStop() {
        if (!teardownStarted.compareAndSet(false, true)) return
        notify("stopping…")
        teardown.execute {
            try {
                stopEverything()
            } finally {
                stopSelf()
            }
        }
    }

    private fun stopEverything() {
        if (!running) return
        running = false

        /* 每一步都计时并报出去：这几个调用各自都可能拖很久，日志里一眼能看出
         * 是哪个慢 —— 「拆流卡住」这种问题不量化就只能靠猜。 */
        val t0 = SystemClock.elapsedRealtime()
        fun step(what: String) =
            notify("stop: $what +${SystemClock.elapsedRealtime() - t0}ms")

        // 先停采集，免得收尾期间还有帧进来。
        camera?.stop(); camera = null
        step("camera")

        mic?.stop();    mic = null
        step("mic")

        // 给每条订阅补结束哨兵 + FIN，再关会话。
        MoqNative.nativeFinishTracks()
        step("finishTracks")

        MoqNative.nativeStop()
        step("nativeStop")

        notify("stopped in ${SystemClock.elapsedRealtime() - t0}ms")
    }

    override fun onDestroy() {
        /* 兜底：系统直接销毁（比如被划掉）时也走同一条路。正常情况下这里
         * 早已由 requestStop() 跑完，AtomicBoolean 会让它直接返回。 */
        requestStop()
        watchdog.shutdown()
        teardown.shutdown()
        if (current === this) current = null
        super.onDestroy()
    }

    private fun startForegroundCompat() {
        val nm = getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            nm.createNotificationChannel(
                NotificationChannel(CHANNEL_ID, "MoQ push", NotificationManager.IMPORTANCE_LOW))
        }

        val open = PendingIntent.getActivity(
            this, 0, Intent(this, MainActivity::class.java),
            PendingIntent.FLAG_IMMUTABLE)

        val notif: Notification =
            (if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O)
                Notification.Builder(this, CHANNEL_ID)
            else
                @Suppress("DEPRECATION") Notification.Builder(this))
                .setContentTitle("MoQ pushing")
                .setContentText("Camera + mic → moq-lite")
                .setSmallIcon(android.R.drawable.presence_video_online)
                .setContentIntent(open)
                .setOngoing(true)
                .build()

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.UPSIDE_DOWN_CAKE) {
            // Android 14+ 必须显式声明类型，且要与 manifest 的声明一致。
            startForeground(NOTIF_ID, notif,
                ServiceInfo.FOREGROUND_SERVICE_TYPE_CAMERA or
                ServiceInfo.FOREGROUND_SERVICE_TYPE_MICROPHONE)
        } else {
            startForeground(NOTIF_ID, notif)
        }
    }
}
