#include "moq_publisher.hpp"

#include "moq_wire.hpp"

#include <uv.h>

extern "C" {
#include "logger.h"
}

#include <cstdio>
#include <cstring>

namespace flv2moq {

namespace {

/* PUBLISH 的 request_id / track 名。取值与 js 客户端保持一致即可，
 * 服务端只把它们回显在日志里，不参与路由（路由靠 namespace 的 app/stream）。 */
constexpr uint64_t kReqCatalog = 0;
constexpr uint64_t kReqVideo = 2;
constexpr uint64_t kReqAudio = 4;

constexpr uint64_t kAliasCatalog = 0;

/* catalog 流的 JSON：服务端 MoqPushSession 不解析它，仅透传给订阅端。
 * 这里给出与 js 客户端同构的最小内容，方便 pull 端识别轨道。 */
std::string CatalogJson(const std::string &app, const std::string &stream) {
    std::string ns = app + "/" + stream;
    std::string j = "{\"tracks\":[";
    j += "{\"name\":\"video\",\"namespace\":\"" + ns +
         "\",\"codec\":\"avc1.64001f\",\"packaging\":\"loc\"},";
    j += "{\"name\":\"audio\",\"namespace\":\"" + ns +
         "\",\"codec\":\"mp4a.40.2\",\"packaging\":\"loc\"}";
    j += "]}";
    return j;
}

const char *SlotName(int slot) {
    switch (slot) {
    case 0: return "control";
    case 1: return "catalog";
    case 2: return "video";
    case 3: return "audio";
    default: return "?";
    }
}

} /* namespace */

MoqPublisher::MoqPublisher() = default;
MoqPublisher::~MoqPublisher() = default;

int MoqPublisher::Start(uv_loop_t *loop, const std::string &host, int port,
                        const std::string &url_path, const std::string &app,
                        const std::string &stream, FlvReader *reader) {
    app_ = app;
    stream_ = stream;
    reader_ = reader;
    /* 默认按实时节奏推送 */

    wt_callbacks_t cb;
    memset(&cb, 0, sizeof(cb));
    cb.on_connect = &MoqPublisher::OnConnect;
    cb.on_stream_open = &MoqPublisher::OnStreamOpen;
    cb.on_stream_data = &MoqPublisher::OnStreamData;
    cb.on_close = &MoqPublisher::OnClose;
    cb.user_data = this;

    cli_ = wt_client_new(loop, &cb);
    if (!cli_) {
        LOG_ERROR("[flv2moq] wt_client_new 失败");
        return -1;
    }

    LOG_INFO("[flv2moq] 连接 %s:%d path=%s", host.c_str(), port,
             url_path.c_str());
    wt_client_connect_path(cli_, host.c_str(), port, url_path.c_str());
    return 0;
}

void MoqPublisher::OnConnect(wt_client_t * /*cli*/, int status, void *user) {
    MoqPublisher *self = static_cast<MoqPublisher *>(user);
    if (!self) return;
    if (status != 0) {
        LOG_ERROR("[flv2moq] 连接/握手失败 (status=%d)", status);
        self->failed_ = true;
        if (self->quit_) *self->quit_ = true;
        return;
    }
    LOG_INFO("[flv2moq] WebTransport 就绪，开始开流");
    self->OpenNextStream();
}

/* 串行开流：每开出一条，在 on_stream_open 里再开下一条。
 * 这样「第几条开出来」就是稳定的流身份。 */
void MoqPublisher::OpenNextStream() {
    if (opened_count_ >= kSlotCount) return;
    wt_client_open_stream(cli_);
}

void MoqPublisher::OnStreamOpen(wt_client_t * /*cli*/, wt_stream_t *st,
                                void *user) {
    MoqPublisher *self = static_cast<MoqPublisher *>(user);
    if (!self) return;
    self->OnOpened(self->opened_count_, st);
}

void MoqPublisher::OnOpened(int slot, wt_stream_t *st) {
    if (slot < 0 || slot >= kSlotCount) return;
    streams_[slot] = st;
    opened_count_ = slot + 1;
    LOG_INFO("[flv2moq] 流就绪 %s", SlotName(slot));

    if (slot == kSlotControl) {
        /* SETUP 必须最先发，服务端 ParseControl 依赖它建立 session 语义 */
        Bytes setup = EncodeSetup();
        SendControl(setup.data(), setup.size(), "SETUP");

        Bytes p1 = EncodePublish(kReqCatalog, app_, stream_, "catalog",
                                 kAliasCatalog);
        Bytes p2 = EncodePublish(kReqVideo, app_, stream_, "video", kAliasVideo);
        Bytes p3 = EncodePublish(kReqAudio, app_, stream_, "audio", kAliasAudio);
        SendControl(p1.data(), p1.size(), "PUBLISH catalog");
        SendControl(p2.data(), p2.size(), "PUBLISH video");
        SendControl(p3.data(), p3.size(), "PUBLISH audio");

        OpenNextStream(); /* → catalog */
        return;
    }

    if (slot == kSlotCatalog) {
        Bytes hdr = EncodeSubgroupHeader(kAliasCatalog, 0);
        SendMedia(slot, hdr.data(), hdr.size(), "SUBGROUP catalog");

        std::string json = CatalogJson(app_, stream_);
        LocProps props;
        props.timestamp_ms = 0;
        Bytes obj = EncodeObject(props,
                                 reinterpret_cast<const uint8_t *>(json.data()),
                                 json.size());
        SendMedia(slot, obj.data(), obj.size(), "OBJECT catalog");

        OpenNextStream(); /* → video */
        return;
    }

    if (slot == kSlotVideo) {
        Bytes hdr = EncodeSubgroupHeader(kAliasVideo, video_group_);
        video_group_open_ = true;
        SendMedia(slot, hdr.data(), hdr.size(), "SUBGROUP video");
        OpenNextStream(); /* → audio */
        return;
    }

    if (slot == kSlotAudio) {
        Bytes hdr = EncodeSubgroupHeader(kAliasAudio, 0);
        SendMedia(slot, hdr.data(), hdr.size(), "SUBGROUP audio");
        media_ready_ = true;
        LOG_INFO("[flv2moq] 信令完成，开始按时间戳推流");
        Pump();
        return;
    }
}

void MoqPublisher::SendControl(const uint8_t *data, size_t len,
                               const char *what) {
    if (!streams_[kSlotControl]) return;
    wt_stream_write_cb(streams_[kSlotControl], data, len,
                       &MoqPublisher::OnWriteDone, this, 5000);
    LOG_DEBUG("[flv2moq] → %s (%zuB)", what, len);
}

void MoqPublisher::SendMedia(int slot, const uint8_t *data, size_t len,
                             const char *what) {
    if (!streams_[slot]) return;
    if (slot == kSlotVideo) inflight_video_++;
    else if (slot == kSlotAudio) inflight_audio_++;
    wt_stream_write_cb(streams_[slot], data, len, &MoqPublisher::OnWriteDone,
                       this, 5000);
    (void)what;
}

void MoqPublisher::OnWriteDone(wt_stream_t *st, int ret, void *user) {
    MoqPublisher *self = static_cast<MoqPublisher *>(user);
    if (!self) return;

    /* 写回调带了流指针，据此分辨视频/音频，各自递减自己的在途计数 */
    if (st == self->streams_[kSlotVideo]) {
        if (self->inflight_video_ > 0) self->inflight_video_--;
    } else if (st == self->streams_[kSlotAudio]) {
        if (self->inflight_audio_ > 0) self->inflight_audio_--;
    }

    if (ret != 0) {
        self->stats_.write_errors++;
        if (self->stats_.write_errors <= 5) {
            LOG_WARN("[flv2moq] 写失败 ret=%d (第 %zu 次)", ret,
                     self->stats_.write_errors);
        }
        if (ret < 0 && !self->failed_) {
            self->failed_ = true;
            if (self->quit_) *self->quit_ = true;
        }
        return;
    }
    self->Pump();
}

void MoqPublisher::OnStreamData(wt_client_t * /*cli*/, wt_stream_t * /*st*/,
                                const uint8_t * /*data*/, size_t /*len*/,
                                void * /*user*/) {
    /* 推送方向不需要处理入站数据 */
}

void MoqPublisher::OnClose(wt_client_t * /*cli*/, int err, void *user) {
    MoqPublisher *self = static_cast<MoqPublisher *>(user);
    if (!self) return;
    LOG_ERROR("[flv2moq] 会话关闭 err=%d", err);
    self->failed_ = true;
    if (self->quit_) *self->quit_ = true;
}

/* 补货：把 reader 已解出的帧搬进 pending_，并保证按 dts 有序。
 * 同一 dts 时视频排在音频前 —— 关键帧先到，订阅端体验更好。 */
void MoqPublisher::Refill() {
    if (!reader_) return;
    for (int guard = 0; guard < 64 && !QueuesFull(); guard++) {
        while (!QueuesFull() && !reader_->Empty()) {
            FlvFrame f = reader_->TakeFront();
            if (f.is_video) video_q_.push_back(std::move(f));
            else audio_q_.push_back(std::move(f));
        }
        if (QueuesFull() || reader_->Eof()) break;
        if (!reader_->Fill()) break;
    }
    if (video_q_.empty() && audio_q_.empty() && reader_->Eof())
        eof_reached_ = true;
}

void MoqPublisher::EnsureAnchor(int64_t first_dts) {
    if (anchor_dts_ >= 0) return;
    anchor_dts_ = first_dts;
    anchor_wall_ms_ = uv_now(uv_default_loop());
}

int64_t MoqPublisher::LatenessOf(int64_t head_dts, uint64_t now_ms) const {
    if (anchor_dts_ < 0) return 0;
    const int64_t elapsed_file = head_dts - anchor_dts_;
    const int64_t due = static_cast<int64_t>(anchor_wall_ms_) +
                        static_cast<int64_t>(elapsed_file / speed_);
    return static_cast<int64_t>(now_ms) - due;
}

/* ── 视频：拥塞时丢帧保住实时性 ──
 * 关键帧不丢（丢了整个 GOP 都没法解），而是把时间轴重锚到它，
 * 让它立刻发出去、延迟从这一帧重新起算。 */
void MoqPublisher::PumpVideo() {
    if (inflight_video_ > 0) return;      /* 等 ACK 推进 */
    if (video_q_.empty()) return;

    const uint64_t now = uv_now(uv_default_loop());
    EnsureAnchor(video_q_.front().dts);

    /* 1) 队首落后超过阈值就丢。关键帧和配置帧（SPS/PPS）不能丢：
     *    关键帧是一整个 GOP 的解码参考，配置帧只有几十字节、
     *    丢了订阅端就永远拿不到 avcC。 */
    for (;;) {
        if (video_q_.empty()) return;
        const int64_t late = LatenessOf(video_q_.front().dts, now);
        if (late > stats_.max_late_ms) stats_.max_late_ms = late;
        if (late <= kMaxLateMs) break;
        if (video_q_.front().key || video_q_.front().is_seq_hdr) break;
        video_q_.pop_front();
        stats_.video_dropped++;
    }

    /* 2) 关键帧仍然落后很多 → 追不上了，重锚到这一帧让它立即发送，
     *    后续帧以它为新的时间起点。 */
    int64_t late = LatenessOf(video_q_.front().dts, now);
    if (video_q_.front().key && late > kResyncLateMs) {
        anchor_dts_ = video_q_.front().dts;
        anchor_wall_ms_ = now;
        late = 0;
    }
    if (late < 0) return;                 /* 还没到点 */

    FlvFrame f = std::move(video_q_.front());
    video_q_.pop_front();
    SendOneVideo(std::move(f));
}

/* ── 音频：连续流，丢单帧听感损失最小，直接丢 ── */
void MoqPublisher::PumpAudio() {
    if (inflight_audio_ > 0) return;
    if (audio_q_.empty()) return;

    const uint64_t now = uv_now(uv_default_loop());
    for (;;) {
        if (audio_q_.empty()) return;
        const int64_t late = LatenessOf(audio_q_.front().dts, now);
        if (late > stats_.max_late_ms) stats_.max_late_ms = late;
        if (late <= kMaxLateMs) break;
        /* ASC 配置帧同样不丢 */
        if (audio_q_.front().is_seq_hdr) break;
        audio_q_.pop_front();
        stats_.audio_dropped++;
    }
    if (LatenessOf(audio_q_.front().dts, now) < 0) return;

    FlvFrame f = std::move(audio_q_.front());
    audio_q_.pop_front();
    SendOneAudio(std::move(f));
}

void MoqPublisher::SendOneVideo(FlvFrame f) {
    LocProps props;
    props.timestamp_ms = static_cast<uint64_t>(f.dts > 0 ? f.dts : 0);
    /* 带上 CTS，服务端才能还原 pts。缺了它 FLV 里 pts 会被写成等于 dts，
     * 有 B 帧的流在 MSE 播放器上就会前后跳（ffplay 靠码流自纠看不出来）。 */
    props.cts = static_cast<uint64_t>(f.cts > 0 ? f.cts : 0);
    props.key = f.key;
    if (f.is_seq_hdr) {
        video_cfg_ = std::move(f.payload);
        return;
    }
    if (!video_cfg_sent_ && !video_cfg_.empty()) {
        props.video_cfg = video_cfg_.data();
        props.video_cfg_len = video_cfg_.size();
        video_cfg_sent_ = true;
    }
    const size_t raw = f.payload.size();
    Bytes obj = EncodeObject(props, f.payload.data(), raw);
    stats_.video_sent++;
    stats_.video_bytes += raw;
    stats_.last_dts = f.dts;
    SendMedia(kSlotVideo, obj.data(), obj.size(), "video");
}

void MoqPublisher::SendOneAudio(FlvFrame f) {
    LocProps props;
    props.timestamp_ms = static_cast<uint64_t>(f.dts > 0 ? f.dts : 0);
    if (f.is_seq_hdr) {
        audio_cfg_ = std::move(f.payload);
        return;
    }
    if (!audio_cfg_sent_ && !audio_cfg_.empty()) {
        props.audio_cfg = audio_cfg_.data();
        props.audio_cfg_len = audio_cfg_.size();
        audio_cfg_sent_ = true;
    }
    const size_t raw = f.payload.size();
    Bytes obj = EncodeObject(props, f.payload.data(), raw);
    stats_.audio_sent++;
    stats_.audio_bytes += raw;
    SendMedia(kSlotAudio, obj.data(), obj.size(), "audio");
}

/* 驱动两支队列：视频与音频各自独立推进。
 * 二者在不同 QUIC 流上，共享 in-flight 计数会让被 cwnd 卡住的视频拖死音频。 */
void MoqPublisher::Pump() {
    if (failed_ || !media_ready_) return;

    Refill();
    PumpVideo();
    PumpAudio();

    /* 收尾：文件读完、两队都空、且没有在途写 → 全部发完了 */
    if (eof_reached_ && video_q_.empty() && audio_q_.empty() &&
        inflight_video_ == 0 && inflight_audio_ == 0) {
        if (quit_) *quit_ = true;
    }
}

} /* namespace flv2moq */
