package com.example.moqlitepush

import android.annotation.SuppressLint
import android.content.Context
import android.content.pm.PackageManager
import android.media.AudioFormat
import android.media.AudioRecord
import android.media.MediaCodec
import android.media.MediaCodecInfo
import android.media.MediaFormat
import android.os.SystemClock
import android.util.Log
import java.nio.ByteBuffer
import java.util.Locale
import kotlin.math.abs

/**
 * 话筒 → AAC。
 *
 * 路径：AudioRecord(PCM 16-bit) ──ByteBuffer──> MediaCodec(audio/mp4a-latm)
 *      ──裸 AAC──> JNI
 *
 * 同样**没有一行「PCM → AAC」**：那是硬件的 AAC 编码器做的，我们只负责把它
 * 配置对、把 PCM 喂进去。MediaCodec 的 AAC 输出是**裸 AAC**（不带 ADTS 头），
 * 正好就是 moq-lite 要的形状。
 *
 * ⚠️ **时间轴与视频必须同源** —— 这一条不显眼但会要命。
 *
 * 视频的 PTS 来自相机：CameraGlRotator 把 `SurfaceTexture.getTimestamp()`
 * （= Camera2 的 SENSOR_TIMESTAMP）经 eglPresentationTimeANDROID 交给编码器。
 * SENSOR_TIMESTAMP 的时基是 **SystemClock.elapsedRealtimeNanos()**
 * （CLOCK_BOOTTIME，**包含**深睡时间），而 `System.nanoTime()` 是
 * CLOCK_MONOTONIC（**不包含**深睡）。手机睡过多久，两个钟的 epoch 就差多少 ——
 * 几十分钟到几小时，远超服务端判「时钟回退」的 1 秒阈值。
 *
 * 后果（实测）：服务端 MediaStream::NormalizeDts 的 last_in_dts_ 是音视频
 * **共用**的，于是每次音视频交替都像一次「时钟回退」，被重新打上 +40ms；
 * 一次交替两个 +40 = 音频 PTS 每 2/3 帧跳 **80ms**（内容只有 21.3ms），
 * 时间轴跑到实时 2.7 倍，播放端按空洞补静音、把真实声音挤出环形缓冲，
 * 听感就是「丢包、不清楚」。flv.js/MSE 因为自己重新排时间轴，症状不明显。
 *
 * 所以音频必须用**和相机同一个钟**：elapsedRealtimeNanos。
 */
class MicEncoder(
    private val context: Context,
    private val onLog: (String) -> Unit,
) {

    companion object {
        private const val TAG = "MoqMic"

        const val SAMPLE_RATE = 48_000

        /* ⚠️ **必须是 2（立体声），不能是 1。** 这不是音质偏好，是 web 播放端的
         * 硬约束 —— 它那边的声道数是**写死**的：
         *
         *   webcodecsPlayer 构造时 AudioPlayer.warmup(48000, 2) 起 AudioContext 与
         *   worklet，PcmRing 按 **2 声道**分配；ASC 到达后 configureAudio() 只更新了
         *   自己那两个字段，**从不重建 worklet**。而 audioPlayer.push() 里
         *       const frames = interleaved.length / this.channels   // channels 恒为 2
         *   于是 mono 的 N 个采样被当成 N/2 帧 ⇒ **时长只剩一半 ⇒ 2 倍速播放**，
         *   听感就是「采样率不对」。
         *
         * 所以单声道源在浏览器里是坏的（web 那侧的问题），而我们这一端**必须发
         * 立体声**才能和它对上。 */
        const val CHANNELS = 2
        /* 立体声了，码率跟着抬一档 —— 64k 给 2 声道偏紧。 */
        const val BITRATE = 128_000

        /** 每次读的采样数。1024 帧 ≈ 21ms，和 AAC 帧长同量级。 */
        private const val READ_FRAMES = 1024
    }

    private var record: AudioRecord? = null
    private var codec: MediaCodec? = null
    private var thread: Thread? = null
    @Volatile private var running = false

    /** 单调、与视频同源的时间轴；偏离墙钟太多（比如进程被挂起过）就重锚。 */
    private var nextTsUs = 0L

    /** 与相机 PTS 同一个时基（CLOCK_BOOTTIME）。**不要**换成 System.nanoTime ——
     *  那是 CLOCK_MONOTONIC，和相机相差整个深睡时间，服务端会把它当成时钟回退。 */
    private fun nowUs(): Long = SystemClock.elapsedRealtimeNanos() / 1000

    /* ── 诊断：证明「采集 → 编码」这一段没有丢采样 ──
     *
     * 浏览器上的 afps ≈ 46.9 只能说明**包数**对（48000 / 1024），说明不了每个包
     * 里的样本是否连续。AudioRecord 的内部缓冲一旦溢出（pump 线程被调度走超过
     * 缓冲时长），它会**静默丢掉最旧的采样**：编码出来的 AAC 帧数、时间戳仍然
     * 连续漂亮，但声音内容已经缺了一块 —— 听感正是「丢包」。
     *
     * 所以这里用两个互相独立的量对账：
     *   fed  = 真正喂进编码器的样本数，按 48k/s 折算成时间
     *   wall = 本统计窗口实际经过的墙钟时间
     * 一个窗口里 fedTime < wallTime（drift 为负）就是采集侧真的丢了采样，
     * 丢多少就是差多少；≈0 说明这一段是完整的。
     *
     * 同时统计编码输出 PTS 的间隔：AAC-LC 一帧 1024 样本，48k 下应恒为
     * ≈21.333ms。它只反映**时间戳**是否连续（浏览器按它判丢帧/补静音），
     * 和 drift 是两件事，要分开看。 */
    private var statAnchorUs = 0L
    private var fedFrames = 0L
    private var outPkts = 0L
    private var outBytes = 0L
    private var lastOutPtsUs = -1L
    private var minPtsDeltaUs = Long.MAX_VALUE
    private var maxPtsDeltaUs = 0L
    /** AudioRecord.read 返回 <=0 / 抛异常的次数（每秒最多报一次）。 */
    private var readFailures = 0L
    private var lastReadWarnUs = 0L

    private fun log(s: String) {
        Log.i(TAG, s)
        onLog(s)
    }

    fun hasPermission(): Boolean =
        context.checkSelfPermission(android.Manifest.permission.RECORD_AUDIO) ==
            PackageManager.PERMISSION_GRANTED

    @SuppressLint("MissingPermission")
    fun start() {
        if (running) return
        if (!hasPermission()) {
            log("RECORD_AUDIO not granted — audio disabled")
            return
        }
        running = true

        try {
            startCodec()
        } catch (t: Throwable) {
            log("audio encoder failed: ${t.message}")
            running = false
            return
        }

        thread = Thread({ pump() }, "moq-aenc").also { it.start() }
        log("audio encoder started: ${SAMPLE_RATE}Hz ${CHANNELS}ch @${BITRATE / 1000}kbps")
    }

    /* Lint 的 MissingPermission 分析是**方法内**的：`start()` 里查过权限，这里
     * 看不见 —— 所以构造 AudioRecord 的调用点会被报「可能没有权限」。
     * 这里显式再查一次（同时挡住 start() 与这里之间权限被撤销的竞态：那种情况
     * AudioRecord 构造会抛 SecurityException），并加注解让 lint 不必做跨方法推断。 */
    @SuppressLint("MissingPermission")
    private fun startCodec() {
        val fmt = MediaFormat.createAudioFormat("audio/mp4a-latm", SAMPLE_RATE, CHANNELS).apply {
            setInteger(MediaFormat.KEY_AAC_PROFILE,
                MediaCodecInfo.CodecProfileLevel.AACObjectLC)
            setInteger(MediaFormat.KEY_BIT_RATE, BITRATE)
            setInteger(MediaFormat.KEY_MAX_INPUT_SIZE, READ_FRAMES * 2 * CHANNELS)
        }
        codec = MediaCodec.createEncoderByType("audio/mp4a-latm").apply {
            configure(fmt, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE)
            start()
        }

        /* getMinBufferSize 返回负数 = **这套参数本机不支持**（ERROR_BAD_VALUE=-2 /
         * ERROR=-1）。早先直接 max(…, 4096) 会把这个错误吞掉，然后建出一个
         * 参数不对的 AudioRecord —— 采集能"跑"，但内容/速率可能是错的。 */
        val minBuf = AudioRecord.getMinBufferSize(
            SAMPLE_RATE, AudioFormat.CHANNEL_IN_STEREO, AudioFormat.ENCODING_PCM_16BIT)
        if (minBuf < 0) {
            throw IllegalStateException(
                "AudioRecord rejects ${SAMPLE_RATE}Hz stereo PCM16 (getMinBufferSize=$minBuf)")
        }
        val bufBytes = maxOf(minBuf, READ_FRAMES * 2 * CHANNELS) * 2

        if (context.checkSelfPermission(android.Manifest.permission.RECORD_AUDIO) !=
            PackageManager.PERMISSION_GRANTED) {
            throw SecurityException("RECORD_AUDIO not granted at AudioRecord creation")
        }

        val rec = AudioRecord(
            android.media.MediaRecorder.AudioSource.DEFAULT,
            SAMPLE_RATE,
            AudioFormat.CHANNEL_IN_STEREO,
            AudioFormat.ENCODING_PCM_16BIT,
            bufBytes,
        )
        record = rec
        if (rec.state != AudioRecord.STATE_INITIALIZED) {
            throw IllegalStateException("AudioRecord init failed (state=${rec.state})")
        }
        rec.startRecording()

        /* ── 采集侧真正生效的参数 ──
         * 我们请求的是 48k/立体声 PCM16，但播放端（web worklet）是**按 48k/2ch
         * 写死**的：AudioContext({sampleRate:48000})、PcmRing 按 2 声道分配、
         * push() 里 frames = interleaved.length / 2。所以这里必须确认设备真的
         * 给了这个形状 —— 不一致时听感就是「音调不对 / 声音断断续续」，
         * 而 afps 仍然会是 46.9（包率只跟编码帧长有关，不看声道/采样率）。 */
        /* ⚠️ AudioRecord.getAudioFormat() 返回的是**编码常量 int**（不是
         * AudioFormat 对象，那个是 getFormat()）。这里要的就是那个 int。 */
        log("audio capture: requested ${SAMPLE_RATE}Hz ${CHANNELS}ch, " +
            "AudioRecord ${rec.sampleRate}Hz ${rec.channelCount}ch " +
            "encoding=${rec.audioFormat} buf=${bufBytes}B")
        if (rec.sampleRate != SAMPLE_RATE || rec.channelCount != CHANNELS) {
            log("WARN: capture negotiated ${rec.sampleRate}Hz ${rec.channelCount}ch " +
                "but the web player is fixed at ${SAMPLE_RATE}Hz ${CHANNELS}ch — " +
                "playback rate/channel mapping will be wrong")
        }
    }

    /** 一个线程同时做两件事：喂 PCM 进去、把 AAC 抽出来。
     *  同步 API（不用 setCallback）在这种单线程泵里更直白。 */
    private fun pump() {
        val pcm = ByteArray(READ_FRAMES * 2 * CHANNELS)
        val info = MediaCodec.BufferInfo()
        statAnchorUs = nowUs()

        while (running) {
            // ① 喂
            val inIdx = try { codec?.dequeueInputBuffer(10_000) ?: -1 } catch (t: Throwable) { -1 }
            if (inIdx >= 0) {
                val n = try { record?.read(pcm, 0, pcm.size) ?: 0 } catch (t: Throwable) {
                    log("WARN: AudioRecord.read threw ${t.javaClass.simpleName}: ${t.message}")
                    0
                }
                val ib = codec?.getInputBuffer(inIdx)
                if (ib != null && n > 0) {
                    ib.clear()
                    ib.put(pcm, 0, n)
                    codec?.queueInputBuffer(inIdx, 0, n, tsFor(n), 0)
                    fedFrames += n.toLong() / 2 / CHANNELS
                } else {
                    /* ⚠️ 读不到数据时这里只是「交空 buffer」——如果 read 一直返回
                     * -3(DEAD_OBJECT)/-1，循环仍然活着，用户看到的是「连通、包率
                     * 正常、但没声音」。所以必须把这个计数打出来。 */
                    readFailures++
                    val now = nowUs()
                    if (now - lastReadWarnUs > 1_000_000L) {
                        lastReadWarnUs = now
                        log("WARN: AudioRecord.read returned $n (failures=$readFailures) — " +
                            "capture is stalled; the encoder is being fed nothing")
                    }
                    // 空 buffer 让编码器继续走，别卡住 dequeue。
                    codec?.queueInputBuffer(inIdx, 0, 0, tsFor(0), 0)
                }
            }

            // ② 抽
            var outIdx = try { codec?.dequeueOutputBuffer(info, 0) ?: -1 } catch (t: Throwable) { -1 }
            while (outIdx >= 0) {
                if (info.size > 0) {
                    val ob = codec?.getOutputBuffer(outIdx)
                    if (ob != null) {
                        val arr = ByteArray(info.size)
                        ob.position(info.offset)
                        ob.limit(info.offset + info.size)
                        ob.get(arr, 0, info.size)
                        MoqNative.nativeSendAudio(info.presentationTimeUs, arr, arr.size)
                        noteOutput(info.presentationTimeUs, info.size)
                    }
                }
                codec?.releaseOutputBuffer(outIdx, false)
                outIdx = try { codec?.dequeueOutputBuffer(info, 0) ?: -1 } catch (t: Throwable) { -1 }
            }

            // ③ csd-0 到齐 = 拿到 AudioSpecificConfig，交给 native 建 catalog
            if (outIdx == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED) {
                publishAudioConfig()
            }
            // INFO_TRY_AGAIN_LATER(-1) / INFO_OUTPUT_BUFFERS_CHANGED(-3) 都是正常的，
            // 什么都不用做，下一轮继续。

            logAudioStats()
        }
    }

    /** 记一帧编码输出（泵线程内调用，无并发）。 */
    private fun noteOutput(ptsUs: Long, size: Int) {
        outPkts++
        outBytes += size
        if (lastOutPtsUs >= 0) {
            val d = ptsUs - lastOutPtsUs
            if (d < minPtsDeltaUs) minPtsDeltaUs = d
            if (d > maxPtsDeltaUs) maxPtsDeltaUs = d
        }
        lastOutPtsUs = ptsUs
    }

    /** 每 5 秒一行采集侧对账日志。判读方法见字段定义处的注释。 */
    private fun logAudioStats() {
        val now = nowUs()
        val wallUs = now - statAnchorUs
        if (wallUs < 5_000_000L) return

        val mediaUs = fedFrames * 1_000_000L / SAMPLE_RATE
        val driftMs = (mediaUs - wallUs) / 1000
        val fedRate = fedFrames * 1_000_000.0 / wallUs
        val pktRate = outPkts * 1_000_000.0 / wallUs
        val kbps = outBytes * 8 * 1_000_000.0 / wallUs / 1000.0
        val ptsDelta = if (minPtsDeltaUs == Long.MAX_VALUE) "n/a"
                       else "[$minPtsDeltaUs,$maxPtsDeltaUs]us"

        log(String.format(
            Locale.US,
            "audio tx: fed=%.0f smp/s out=%.1f pkt/s %.0f kbps pts_delta=%s drift=%dms read_fail=%d",
            fedRate, pktRate, kbps, ptsDelta, driftMs, readFailures))
        if (driftMs < -100) {
            log(String.format(
                Locale.US,
                "WARN: audio capture fed %d ms less than wall clock in this window — " +
                "AudioRecord buffer overrun, samples were dropped",
                -driftMs))
        }
        if (minPtsDeltaUs != Long.MAX_VALUE && maxPtsDeltaUs - minPtsDeltaUs > 2_000L) {
            log(String.format(
                Locale.US,
                "WARN: audio pts delta varies by %d us (nominal 21333us) — " +
                "the player may pad silence / judge frames missing",
                maxPtsDeltaUs - minPtsDeltaUs))
        }

        statAnchorUs = now
        fedFrames = 0
        outPkts = 0
        outBytes = 0
        minPtsDeltaUs = Long.MAX_VALUE
        maxPtsDeltaUs = 0
    }

    /** 这一批采样第一帧的绝对微秒。**单调**，不再随墙钟来回跳。
     *
     * 原来每块都拿墙钟重算一次，偏差超过 200ms 就重锚 —— 问题是编码器/调度稍有
     * 抖动就会触发，时间轴于是出现**凭空的一段空洞**。音频样本本身一个不少地
     * 送到了，播放端却按时间戳判断「丢了帧」，于是补静音；补的静音和真实音频
     * 挤同一个环形缓冲，反而把**真实语音**顶出去 —— 听感就是「丢包、断断续续」。
     * （实测：服务端 47 帧/秒全收到了，播放端却补了 208 秒静音、丢了 232 秒真实音频。）
     *
     * 这里改成**只锚一次**（第一块用墙钟定原点），之后按样本数**累加**：48kHz 是
     * 恒定速率，时间轴天然连续。只有当确实停了很久（>2s，比如被系统冻结）才重锚，
     * 那种情况下的时间跳变是真实的、播放端也 clamp 得住。 */
    private fun tsFor(bytes: Int): Long {
        val frames = bytes / 2 / CHANNELS
        val anchored = nowUs() - frames.toLong() * 1_000_000L / SAMPLE_RATE
        if (nextTsUs == 0L || abs(anchored - nextTsUs) > 2_000_000L) {
            nextTsUs = anchored
        }
        val ts = nextTsUs
        nextTsUs += frames.toLong() * 1_000_000L / SAMPLE_RATE
        return ts
    }

    private fun publishAudioConfig() {
        try {
            val fmt = codec?.outputFormat ?: return
            val csd0 = fmt.getByteBuffer("csd-0") ?: run {
                log("audio format changed but csd-0 missing")
                return
            }
            val asc = ByteBuffer.allocate(csd0.remaining()).also { b -> b.put(csd0).flip() }.array()

            /* 编码器**实际**采用的参数。我们请求的是 48k/2ch，但设备可以协商成别的
             * （尤其是采样率），而播放端按 ASC 解 —— 所以这里把两边都打出来对照。 */
            val encRate = if (fmt.containsKey(MediaFormat.KEY_SAMPLE_RATE))
                fmt.getInteger(MediaFormat.KEY_SAMPLE_RATE) else -1
            val encCh = if (fmt.containsKey(MediaFormat.KEY_CHANNEL_COUNT))
                fmt.getInteger(MediaFormat.KEY_CHANNEL_COUNT) else -1

            /* catalog 里报的采样率/声道数改用 **ASC 解出来的值**，而不是我们请求的
             * 常量：ASC 才是解码器真正按它解的那份参数，catalog 报另一个数只会让
             * 下游（FLV/MSE 那条线）以为流是 48k/2ch。解析失败才退回常量。 */
            val ascSrate = ascRate(asc)
            val ascCh = ascChannels(asc)
            val catalogRate = if (ascSrate > 0) ascSrate else SAMPLE_RATE
            val catalogCh = if (ascCh in 1..8) ascCh else CHANNELS
            val rc = MoqNative.nativeAudioConfig(asc, catalogRate, catalogCh, "1.aac")

            log("audio config: ASC ${asc.size} B [${ascInfo(asc)}] -> native rc=$rc " +
                "(encoder rate=$encRate ch=$encCh, catalog rate=$catalogRate ch=$catalogCh)")
            if (ascSrate > 0 && ascSrate != SAMPLE_RATE) {
                log("WARN: ASC rate $ascSrate != requested $SAMPLE_RATE — " +
                    "the web player creates its AudioContext at 48kHz; playing 44.1k PCM " +
                    "there runs 8.8% fast and the ring underruns")
            }
            if (ascCh in 1..8 && ascCh != CHANNELS) {
                log("WARN: ASC channels $ascCh != requested $CHANNELS — mono is currently " +
                    "broken on the web player (worklet is fixed at 2ch)")
            }
        } catch (t: Throwable) {
            Log.e(TAG, "publishAudioConfig failed", t)
        }
    }

    /** ASC 里的采样率索引 → 实际采样率；解析不出来返回 -1。 */
    private fun ascRate(asc: ByteArray): Int {
        if (asc.size < 2) return -1
        val b0 = asc[0].toInt() and 0xff
        val b1 = asc[1].toInt() and 0xff
        val idx = ((b0 and 0x07) shl 1) or ((b1 shr 7) and 0x01)
        val rates = intArrayOf(96000, 88200, 64000, 48000, 44100, 32000, 24000,
                               22050, 16000, 12000, 11025, 8000, 7350)
        return if (idx in rates.indices) rates[idx] else -1
    }

    /** ASC 里的声道数；解析不出来返回 -1。显式 24bit 采样率时声道数在字节 4 里。 */
    private fun ascChannels(asc: ByteArray): Int {
        if (asc.size < 2) return -1
        val b0 = asc[0].toInt() and 0xff
        val b1 = asc[1].toInt() and 0xff
        val idx = ((b0 and 0x07) shl 1) or ((b1 shr 7) and 0x01)
        return if (idx == 15) {
            if (asc.size < 5) -1 else (asc[4].toInt() shr 3) and 0x0f
        } else {
            (b1 shr 3) and 0x0f
        }
    }

    /** 把 AudioSpecificConfig 的关键字段打出来（标准布局：5/4/4 位）。 */
    private fun ascInfo(asc: ByteArray): String {
        if (asc.size < 2) return "too short (${asc.size}B)"
        val b0 = asc[0].toInt() and 0xff
        val aot = (b0 shr 3) and 0x1f
        val hex = asc.joinToString("") { String.format(Locale.US, "%02x", it) }
        return "aot=$aot rate=${ascRate(asc)} ch=${ascChannels(asc)} hex=$hex"
    }

    /** ⚠️ 只能在**非主线程**上调（PushService 有一条专门的拆流线程）：
     *  AudioRecord/MediaCodec 的关闭都可能阻塞，跑在主线程上就是 ANR。 */
    fun stop() {
        running = false
        val t0 = SystemClock.elapsedRealtime()
        fun step(what: String) =
            log("stop: $what +${SystemClock.elapsedRealtime() - t0}ms")

        /* 先等采集线程收手再放掉 AudioRecord/MediaCodec —— 反过来的话那个线程
         * 可能正卡在 read()/dequeue 里，用着已经释放的对象。 */
        try { thread?.join(1000) } catch (_: InterruptedException) {}
        thread = null
        step("pump joined")

        try { record?.stop() } catch (_: Throwable) {}
        try { record?.release() } catch (_: Throwable) {}
        record = null
        step("record released")

        try { codec?.stop() } catch (_: Throwable) {}
        try { codec?.release() } catch (_: Throwable) {}
        codec = null
        step("codec released")
    }
}
