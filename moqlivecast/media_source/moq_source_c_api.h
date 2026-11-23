#ifndef MOQLIVECAST_MOQ_SOURCE_C_API_H
#define MOQLIVECAST_MOQ_SOURCE_C_API_H

/*
 * moqlivecast —— 媒体源 C API
 *
 * 用途：让"已编码音视频"的直接生产者（摄像头 SDK、硬件编码器、DVR 驱动）
 * 以 C ABI 把帧送进 moqlivecast，不需要文件、FIFO 或轮询。
 *
 * 典型用法（摄像头进程内）：
 *
 *     moq_source_t* s = moq_source_create("anon", "cam0", "cam0");
 *     moq_source_start(s);
 *
 *     // 编码器初始化后
 *     moq_source_push_video_config(s, MOQ_CODEC_H264, sps_pps, sps_pps_len);
 *
 *     // 每帧
 *     moq_video_frame_t f = {0};
 *     f.data = buf; f.len = n;
 *     f.dts_ms = dts; f.pts_ms = pts;
 *     f.is_keyframe = key;
 *     f.format = MOQ_BITSTREAM_ANNEXB;
 *     moq_source_push_video(s, &f);
 *
 * 线程约定：
 *   - moq_source_create/start/stop/destroy 必须在 moqlivecast 的 uv loop 线程调用
 *     （由服务端在启动时通过 moq_source_set_loop() 注入）。
 *   - moq_source_push_* 可以从任意线程调用，内部只入队 + uv_async_send，不会阻塞。
 *   - stop() 之前，调用方必须先停止所有 push_* 调用。
 *
 * 版本：1
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MOQ_SOURCE_API_VERSION 1

typedef struct moq_source moq_source_t;

typedef enum {
    MOQ_CODEC_H264 = 0,
    MOQ_CODEC_H265 = 1,
    MOQ_CODEC_AAC  = 2
} moq_codec_t;

/* 视频码流打包方式 */
typedef enum {
    MOQ_BITSTREAM_ANNEXB = 0,   /* 00 00 01 / 00 00 00 01 起始码 */
    MOQ_BITSTREAM_AVCC   = 1    /* AVCC（长度前缀），FLV/MP4 原生格式 */
} moq_bitstream_t;

/* 音频打包方式（与视频无关） */
typedef enum {
    MOQ_AUDIO_AAC_RAW  = 0,     /* 裸 AAC 帧，无 ADTS 头（FLV 原生格式） */
    MOQ_AUDIO_AAC_ADTS = 1      /* 带 ADTS 头；本 API 会剥离并自动推导 AudioSpecificConfig */
} moq_audio_format_t;

typedef struct {
    const uint8_t* data;
    size_t         len;
    int64_t        dts_ms;
    int64_t        pts_ms;
    int            is_keyframe;
    moq_bitstream_t format;
} moq_video_frame_t;

typedef struct {
    uint64_t frames_in;
    uint64_t frames_dropped;   /* 因 -EAGAIN 被调用方丢弃或拒绝的帧 */
    int64_t  last_dts_ms;
    int      running;
} moq_source_stats_t;

/* 服务端启动时调用一次，注入 uv loop。返回 0 成功。 */
int moq_source_set_loop(void* uv_loop);

/* 创建/销毁。app/stream 决定发布到哪条流。 */
moq_source_t* moq_source_create(const char* app, const char* stream,
                                const char* source_id);
void          moq_source_destroy(moq_source_t* s);

/* 启动/停止。start 内部会 ClaimPublisher 并等待 catalog 所需信息。 */
int  moq_source_start(moq_source_t* s);
void moq_source_stop(moq_source_t* s);

/* 编码配置（SPS/PPS、AudioSpecificConfig）。必须在第一帧之前调用；
 * 码流参数变化时可以再次调用。 */
int moq_source_push_video_config(moq_source_t* s, moq_codec_t codec,
                                 const uint8_t* extradata, size_t len);
int moq_source_push_audio_config(moq_source_t* s,
                                 const uint8_t* extradata, size_t len);

/* 推帧。返回 0 表示已接受；-EAGAIN 表示队列满（调用方决定丢帧或重试）；
 * 其它负值为错误。 */
int moq_source_push_video(moq_source_t* s, const moq_video_frame_t* f);
int moq_source_push_audio(moq_source_t* s, const uint8_t* data, size_t len,
                          int64_t dts_ms, int64_t pts_ms,
                          moq_audio_format_t format);

void moq_source_get_stats(moq_source_t* s, moq_source_stats_t* out);

/* 最近一次错误描述（线程安全，静态缓冲，仅在出错后立即读取有效）。 */
const char* moq_source_last_error(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* MOQLIVECAST_MOQ_SOURCE_C_API_H */
