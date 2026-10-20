package com.example.moqlitepush

/**
 * moq-lite C API 的 Kotlin 入口。
 *
 * 分工：**这一层只管取数据**（Camera2 / AudioRecord / MediaCodec 的输出），
 * 协议与比特流都在 C 侧 —— 所以这里看不到任何 moq-lite 的类型。
 *
 * 方法名与 cpp/moq_jni.c 里的 `Java_com_example_moqlitepush_MoqNative_*` 逐字对应。
 * 改名/改包名要两边一起改，否则 JVM 抛 UnsatisfiedLinkError。
 */
object MoqNative {

    init {
        // 对应 CMake 的 target `moqlitepush` → libmoqlitepush.so
        System.loadLibrary("moqlitepush")
    }

    // ── 原生日志 ─────────────────────────────────

    /**
     * 原生日志的出口。收到的是两类日志：**moqlite/QUIC 库自己的**（在 Android
     * 上原本只落文件、Logcat 里一个字看不到）和本目录 moq_pusher 的。
     *
     * ⚠️ **在 native 线程上被调用**（loop 线程 / 库的日志线程 / 编解码回调线程），
     * 不是主线程 —— 别在这里碰 Compose 状态，也别做重活。用 [NativeLog] 这类
     * 线程安全缓冲收着，再让 UI 定期取快照。
     */
    @Volatile
    var onLog: ((String) -> Unit)? = null

    /** C 侧的回调入口。`@JvmStatic` 才能被 JNI 用静态方法调用（见 moq_jni.c）。 */
    @JvmStatic
    fun emitNativeLog(line: String) {
        onLog?.invoke(line)
    }

    /**
     * 开/关原生日志转发。关闭时 C 侧一个字节都不拷、一次 JNI 都不过。
     * 打开前先把 [onLog] 设好。
     */
    external fun nativeSetLogForwarding(on: Boolean)

    /** 库实现的 moq-lite 版本，形如 "moq-lite-06"。 */
    external fun version(): String

    /** 自检（不碰网络）：0 = 通过，<0 = 失败码。 */
    external fun selfTest(): Int

    // ── 生命周期 ─────────────────────────────────

    /**
     * 起推流。连上之后会自动 publish(broadcast)，并在两条轨的编解码参数到齐时
     * 各自 publish_track。
     *
     * [url] 形如 `moql://192.168.1.221:4433/anon`。
     * 域名和 IP 都能填（库会解析）。**但连本地这台服务端时要填 IP**：
     * `www.webrtcserver.com.cn` 指向 127.0.0.1 靠的是**开发机**的 /etc/hosts，
     * 手机上解析不到那条记录 —— 这是环境问题，不是库的限制。
     * ⚠️ 解析只取 IPv4（本仓 QUIC 发送路径写死 IPv4）。
     *
     * [logPath] 非空时把库自己的日志写进该文件（native stdout 在 Android 上
     * 不可见，只能落文件再 `adb pull`）。
     *
     * @return 0 = 已启动，<0 = 失败
     */
    external fun nativeStart(url: String, broadcast: String, logPath: String?): Int

    /** 停：关会话、停 loop 线程。幂等。 */
    external fun nativeStop()

    /** 收尾但不断连接：给每条订阅的当前组补结束哨兵 + FIN。 */
    external fun nativeFinishTracks()

    // ── 编解码参数（必须在帧之前）─────────────────

    /** 视频参数。[avcc] 是拼好的 avcC 记录，见 [nativeBuildAvcC]。 */
    external fun nativeVideoConfig(avcc: ByteArray, width: Int, height: Int, track: String): Int

    /** 音频参数。[asc] 就是 MediaCodec 的 csd-0。 */
    external fun nativeAudioConfig(asc: ByteArray, sampleRate: Int, channels: Int, track: String): Int

    /** 由 MediaCodec 的 csd-0/csd-1 拼 avcC。失败（缺 SPS/PPS）返回 null。 */
    external fun nativeBuildAvcC(csd0: ByteArray, csd1: ByteArray?): ByteArray?

    // ── 帧 ───────────────────────────────────────

    /**
     * 视频一帧。[annexb] 是 MediaCodec 原样吐出来的 Annex-B，C 侧转 AVCC。
     *
     * [len] 是这一帧的**实际字节数** —— 上层复用同一个 ByteArray 接输出，
     * 数组容量通常大于本帧长度，不传 len 会把上一帧的残留一起发出去。
     *
     * ⚠️ 必须在 `releaseOutputBuffer` **之前**调（buffer 交还后就读不到了）。
     */
    external fun nativeSendVideo(tsUs: Long, annexb: ByteArray, len: Int, key: Boolean): Int

    /** 音频一帧。[aac] 是 MediaCodec 的裸 AAC（无 ADTS 头）。[len] 含义同上。 */
    external fun nativeSendAudio(tsUs: Long, aac: ByteArray, len: Int): Int

    // ── 统计 ─────────────────────────────────────

    private const val STATS_LEN = 6
    private val scratch = LongArray(STATS_LEN)

    /** 读一次统计（内部复用同一个 long[]，别在多线程同时调）。 */
    external fun nativeGetStats(out: LongArray): Int

    data class Stats(
        val running: Boolean,
        val connected: Boolean,
        val published: Boolean,
        val videoFrames: Long,
        val audioFrames: Long,
        val dropped: Long,
    )

    fun stats(): Stats {
        if (nativeGetStats(scratch) != 0) {
            return Stats(false, false, false, 0, 0, 0)
        }
        return Stats(
            running      = scratch[0] != 0L,
            connected    = scratch[1] != 0L,
            published    = scratch[2] != 0L,
            videoFrames  = scratch[3],
            audioFrames  = scratch[4],
            dropped      = scratch[5],
        )
    }
}
