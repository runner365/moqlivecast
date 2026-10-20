package com.example.moqlitepush

import java.util.concurrent.ConcurrentLinkedDeque

/**
 * 原生日志的**线程安全**环形缓冲。
 *
 * 为什么要有它：C 侧的日志是在 **native 线程**上回调过来的（libuv 的 loop 线程、
 * 库自己的日志线程、MediaCodec 回调线程），而 Compose 的状态只能在主线程上改。
 * 与其在每个回调里切线程（每行一次 post，量大的时候很浪费），不如先丢进这个
 * 无锁队列，UI 每 500ms 取一次快照 —— 反正界面本来就是 500ms 刷一次。
 *
 * 只保留最近的若干行：日志是拿来「看最近出了什么事」的，
 * 全量在 files/moq_lite.log（adb pull 出来）。
 */
object NativeLog {

    /** 缓冲行数。够看最近一段现场，又不会让 UI 每帧重绘上千行。 */
    private const val CAPACITY = 300

    private val lines = ConcurrentLinkedDeque<String>()

    /** 供 UI 显示的行数。 */
    const val VISIBLE = 14

    fun add(line: String) {
        lines.addLast(line)
        while (lines.size > CAPACITY) lines.pollFirst()
    }

    /** 最近 [n] 行（时间序）。 */
    fun tail(n: Int = VISIBLE): List<String> = lines.toList().takeLast(n)

    fun clear() = lines.clear()
}
