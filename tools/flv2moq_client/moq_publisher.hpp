#ifndef FLV2MOQ_MOQ_PUBLISHER_HPP
#define FLV2MOQ_MOQ_PUBLISHER_HPP

/* ============================================
 * 把 FLV 帧按时间戳推送到 moqlivecast /moq
 *
 * 流布局（与服务端 MoqPushSession 及 js 客户端一致）：
 *   第 1 条 bidi = control  : SETUP + PUBLISH×3
 *   第 2 条 bidi = catalog  : SUBGROUP(alias=0) + JSON 对象（仅一条）
 *   第 3 条 bidi = video    : SUBGROUP(alias=1) + OBJECT×N
 *   第 4 条 bidi = audio    : SUBGROUP(alias=2) + OBJECT×N
 *
 * 顺序不能变：wt_client 的 on_stream_open 只给一个不透明的 wt_stream_t*，
 * 不带任何标识，只能靠「第几条开出来的流」来区分。
 * ============================================ */

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

#include "flv_reader.hpp"

extern "C" {
#include "webtransport_client_api.h"
}

namespace flv2moq {

struct Stats {
    size_t video_sent = 0;
    size_t audio_sent = 0;
    size_t video_bytes = 0;
    size_t audio_bytes = 0;
    size_t write_errors = 0;
    /* 拥塞/落后导致的丢弃 */
    size_t video_dropped = 0;
    size_t audio_dropped = 0;
    int64_t last_dts = 0;
    int64_t max_late_ms = 0;   /* 观察到的最大落后量，用于判断链路是否够用 */
};

class MoqPublisher {
public:
    MoqPublisher();
    ~MoqPublisher();

    MoqPublisher(const MoqPublisher &) = delete;
    MoqPublisher &operator=(const MoqPublisher &) = delete;

    /* 连接并完成 SETUP/PUBLISH。返回 0 表示任务已入队（异步）。 */
    int Start(uv_loop_t *loop, const std::string &host, int port,
              const std::string &url_path, const std::string &app,
              const std::string &stream, FlvReader *reader);

    void SetQuitFlag(bool *flag) { quit_ = flag; }
    /* 发送倍速：>1 快于实时，<1 慢于实时。影响文件 dts → 墙钟的换算。 */
    void SetSpeed(double s) { speed_ = (s > 0.0) ? s : 1.0; }
    const Stats &stats() const { return stats_; }
    bool ready() const { return media_ready_; }
    bool failed() const { return failed_; }

    /* 由 main 的定时器周期性调用，推进发送节拍 */
    void Tick() { Pump(); }

    static void OnConnect(wt_client_t *cli, int status, void *user);
    static void OnStreamOpen(wt_client_t *cli, wt_stream_t *st, void *user);
    static void OnStreamData(wt_client_t *cli, wt_stream_t *st,
                             const uint8_t *data, size_t len, void *user);
    static void OnClose(wt_client_t *cli, int err, void *user);
    static void OnWriteDone(wt_stream_t *st, int ret, void *user);

private:
    /* 流布局（顺序即 open 的先后）。draft-ietf-moq-transport §3.3：
     *   控制消息 → 一对单向流（本端这条只发 SETUP）
     *   请求     → 每条一个双向流（PUBLISH 各占一条）
     *   对象     → 单向流（本阶段暂沿用现有数据流，B 阶段再改） */
    enum StreamSlot {
        kSlotControl = 0,   /* uni  : SETUP */
        kSlotReqCatalog,    /* bidi : PUBLISH catalog */
        kSlotReqVideo,      /* bidi : PUBLISH video */
        kSlotReqAudio,      /* bidi : PUBLISH audio */
        kSlotCatalog,       /* data : SUBGROUP + OBJECT */
        kSlotVideo,
        kSlotAudio,
        kSlotCount
    };

    /* 待发队列上限（帧数）：纯粹是内存保护，不是延迟控制手段。
     * 延迟由 kMaxLateMs 控制。 */
    static const size_t kMaxPending = 512;

    /* 落后调度超过这个量就开始丢帧。
     * 太小 → 轻微抖动就丢帧；太大 → 延迟累积、画面跳变。
     * 500ms 大致是「人眼可察觉卡顿」的门槛，可作为起点调。 */
    static const int64_t kMaxLateMs = 500;

    /* 落后超过这个量、且队首是关键帧 → 重锚时间轴而不是继续补帧。
     * 说明已经追不上了，硬补只会无限落后。 */
    static const int64_t kResyncLateMs = 3000;

    void OnOpened(int slot, wt_stream_t *st);
    void SendControl(int slot, const uint8_t *data, size_t len,
                     const char *what);
    void SendMedia(int slot, const uint8_t *data, size_t len, const char *what);

    /* 视频与音频各自独立推进 —— 二者在不同 QUIC 流上，
     * 共用 in-flight 计数会让被 cwnd 卡住的视频拖死音频。 */
    void PumpVideo();
    void PumpAudio();

    void Pump();          /* 驱动两支队列 + 收尾判定 */
    void Refill();        /* 从 reader 搬运已解出的帧到待发队列 */
    void OpenNextStream();/* 串行开流，保证顺序 */

    /* 首次出帧时锚定「文件 dts ↔ 墙钟」，之后按它算每帧的到期时刻 */
    void EnsureAnchor(int64_t first_dts);
    /* 当前队首相对调度落后多少毫秒（>0 表示已经晚了） */
    int64_t LatenessOf(int64_t head_dts, uint64_t now_ms) const;
    /* 帧数是否已达上限 */
    bool QueuesFull() const {
        return video_q_.size() + audio_q_.size() >= kMaxPending;
    }

    /* 单帧发送（含配置帧的挂载与统计） */
    void SendOneVideo(FlvFrame f);
    void SendOneAudio(FlvFrame f);

    wt_client_t *cli_ = nullptr;
    wt_stream_t *streams_[kSlotCount] = {};
    int opened_count_ = 0;
    bool media_ready_ = false;
    bool failed_ = false;

    std::string app_;
    std::string stream_;
    FlvReader *reader_ = nullptr;
    bool *quit_ = nullptr;

    /* 视频/音频各自独立的待发队列，按 dts 有序 */
    std::deque<FlvFrame> video_q_;
    std::deque<FlvFrame> audio_q_;
    bool eof_reached_ = false;

    /* 采样进度的锚点：文件时间 → 墙钟时间 */
    int64_t anchor_dts_ = -1;
    uint64_t anchor_wall_ms_ = 0;
    double speed_ = 1.0;

    /* 配置只带一次 */
    std::vector<uint8_t> video_cfg_;
    std::vector<uint8_t> audio_cfg_;
    bool video_cfg_sent_ = false;
    bool audio_cfg_sent_ = false;

    uint64_t video_group_ = 0;
    bool video_group_open_ = false;

    /* 每支队列各自的在途写计数 —— 视频被 cwnd 卡住时音频仍能推进 */
    int inflight_video_ = 0;
    int inflight_audio_ = 0;
    Stats stats_;
};

} /* namespace flv2moq */

#endif /* FLV2MOQ_MOQ_PUBLISHER_HPP */
