#ifndef MOQ_PUSHER_H
#define MOQ_PUSHER_H

/* ============================================================
 * 推流的 native 侧：一条 uv loop 线程 + 一个跨线程的帧队列。
 *
 * ⚠️ 为什么要单独一条线程：moq-lite 的 C API 要求 `moq_lite_track_frame` 在
 * **loop 线程**上调用，而 MediaCodec 的输出回调跑在**编解码器自己的线程**上。
 * 两个线程不是同一个，所以中间必须有队列 + `uv_async_send` 把它交接过去。
 * 直接把编解码回调里拿到的指针丢给 API 是错的（buffer 交还 MediaCodec 之后就
 * 失效了），所以入队时**拷一份**。
 *
 * 上层（Kotlin）只看这个头文件，通过 JNI 调它 —— 见 moq_jni.c。
 * 线程安全：所有函数都可以从任意线程调用。内部加锁。
 * ============================================================ */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 进程级初始化：把互斥量与信号量建好。**在库加载时调一次**
 * （moq_jni.c 的 JNI_OnLoad）。在那之前任何函数都不该被调用 —— 之后所有
 * 入口都安全，包括 start 之前的 get_stats。返回 0 / -1。 */
int  moq_pusher_global_init(void);

/* 起推流：建 loop 线程、连 url、连上后自动 publish(broadcast)。
 * 重复调用是空操作（已经在跑就直接返回 0）。返回 0 = 已启动，<0 = 失败。
 *
 * 停掉之后可以再 start：每次 run 的状态会被重置，互斥量/信号量**不重建**
 * （重建会与仍在轮询 get_stats 的 UI 线程抢）。 */
int  moq_pusher_start(const char *url, const char *broadcast);

/* 停：关会话、停 loop、join 线程。幂等。之后再 start 会重新起一条。 */
void moq_pusher_stop(void);

/* 编码器的编解码参数 —— 也就是 catalog 里那条 rendition 的 description。
 *
 * ⚠️ 这两条**必须**在帧之前到达：没有它就没有 catalog，而「没有 catalog 的
 * 广播对端既不被公告、也解不开」。C 侧收到后就地 publish_track。
 *
 * 视频：avcc 是 Annex-B 的 SPS/PPS（用 build_avcc_from_csd() 转成 avcC 再传进来）。
 * 音频：asc 是 MediaCodec 的 csd-0（就是 AudioSpecificConfig 原字节）。
 * codec 串由 C 侧按类型自己拼（avc1.PPCCLL / mp4a.40.2），不麻烦上层。 */
int moq_pusher_set_video_config(const uint8_t *avcc, size_t len,
                                uint32_t width, uint32_t height,
                                const char *track_name);
int moq_pusher_set_audio_config(const uint8_t *asc, size_t len,
                                uint32_t sample_rate, uint32_t channels,
                                const char *track_name);

/* 一帧。任意线程可调；内部拷一份入队，由 loop 线程写出。
 * ts_us 是**绝对微秒**（MediaCodec 的 presentationTimeUs 正好是这个单位）。
 * key 只对视频有意义：关键帧传 1。 */
void moq_pusher_send_video(uint64_t ts_us, const uint8_t *avcc, size_t len, int key);
void moq_pusher_send_audio(uint64_t ts_us, const uint8_t *aac, size_t len);

/* 收尾：给每条订阅补结束哨兵 + FIN。服务停之前调一次。 */
void moq_pusher_finish_tracks(void);

typedef struct {
    int      running;       /* loop 线程在跑 */
    int      connected;     /* 握手完成 */
    int      published;     /* 广播已公告、至少一条轨已声明 */
    uint64_t video_frames;  /* 真正交给 API 的帧数 */
    uint64_t audio_frames;
    uint64_t dropped;       /* 队列满 / 轨还没就绪而丢掉的帧 */
} moq_pusher_stats_t;

/* 读统计（线程安全，给 UI 轮询用）。 */
void moq_pusher_get_stats(moq_pusher_stats_t *out);

#ifdef __cplusplus
}
#endif

#endif /* MOQ_PUSHER_H */
