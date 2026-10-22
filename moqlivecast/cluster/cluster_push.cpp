#include "cluster_push.hpp"

#include "flv_tag_body.hpp"
#include "logger.h"
#include "media_stream_manager.hpp"

#include <algorithm>

namespace cpp_streamer {

namespace {
/* 本地 unpublish 之后保留连接的宽限期：覆盖推流端“断开—重连”的几秒窗口。
 * 期间 catalog 回来就原地恢复，不必重建连接、中心也不必重新发现广播。 */
constexpr int64_t kPauseGraceMs = 30000;
/* on_close 之后隔一拍再释放会话（绝不在 C 回调内部 free）。 */
constexpr int64_t kFreeDelayMs = 100;
} /* namespace */

class ClusterPushLink::Writer : public AvWriterInterface {
public:
    Writer(ClusterPushLink *link, std::string key)
        : link_(link), key_(std::move(key)),
          id_("cluster-push:" + key_) {}

    int WritePacket(Media_Packet_Ptr pkt) override {
        return link_->WritePacket(pkt);
    }
    std::string GetKey() override { return key_; }
    std::string GetWriterId() override { return id_; }
    void CloseWriter() override { closed_ = true; }
    bool IsInited() override { return init_flag_; }
    void SetInitFlag(bool flag) override { init_flag_ = flag; }

private:
    ClusterPushLink *link_ = nullptr;
    std::string key_;
    std::string id_;
    bool init_flag_ = false;
    bool closed_ = false;
};

ClusterPushLink::ClusterPushLink(uv_loop_t *loop, const ClusterOptions &opts,
                                 std::string local_key, std::string wire,
                                 std::string catalog_json)
    : loop_(loop),
      opts_(opts),
      local_key_(std::move(local_key)),
      wire_(std::move(wire)),
      catalog_json_(std::move(catalog_json)),
      writer_(new Writer(this, local_key_)) {
    ParseMoqCatalog(catalog_json_, &catalog_);
    retry_at_ms_ = ClusterNowMs();
}

ClusterPushLink::~ClusterPushLink() {
    DetachWriter();
    if (session_) {
        moq_lite_session_close(session_);
        session_ = 0;
    }
    delete writer_;
    writer_ = nullptr;
}

void ClusterPushLink::Start() {
    if (stopped_) return;
    if (session_) return;
    CreateSession();
}

void ClusterPushLink::CreateSession() {
    if (session_ || stopped_) return;
    session_ = moq_lite_session_new(loop_);
    if (!session_) {
        LOG_ERROR("[cluster] push %s: cannot create moq-lite session",
                  local_key_.c_str());
        retry_at_ms_ = ClusterNowMs() + ClusterBackoffMs(opts_, attempt_,
                                                         local_key_,
                                                         ClusterNowMs());
        return;
    }
    moq_lite_session_set_role(session_, MOQ_LITE_ROLE_PUBLISHER);

    moq_lite_session_cb_t cb;
    cb.on_connected = &ClusterPushLink::OnSessionConnected;
    cb.on_announce = nullptr;
    cb.on_announce_end = nullptr;
    cb.on_close = &ClusterPushLink::OnSessionClosed;
    cb.user = this;

    const std::string url = CenterUrl(opts_);
    connecting_ = true;
    if (moq_lite_session_connect(session_, url.c_str(), &cb) < 0) {
        LOG_ERROR("[cluster] push %s: connect %s failed: %s",
                  local_key_.c_str(), url.c_str(), moq_lite_strerror(-1));
        moq_lite_session_close(session_);
        connecting_ = false;
        /* 等 on_close 把 session 释放掉，再退避重连。 */
        close_pending_ = true;
        free_at_ms_ = ClusterNowMs() + kFreeDelayMs;
        retry_at_ms_ = ClusterNowMs() + ClusterBackoffMs(opts_, attempt_,
                                                         local_key_,
                                                         ClusterNowMs());
        return;
    }
    LOG_INFO("[cluster] push %s -> %s (broadcast \"%s\") connecting",
             local_key_.c_str(), url.c_str(), wire_.c_str());
}

void ClusterPushLink::OnSessionConnected(moq_lite_session_t s, int status,
                                         void *user) {
    (void)s;
    ClusterPushLink *self = static_cast<ClusterPushLink *>(user);
    if (!self || self->stopped_) return;
    if (status != 0) {
        LOG_WARN("[cluster] push %s: connect failed (status=%d), will retry",
                 self->local_key_.c_str(), status);
        self->connecting_ = false;
        if (self->session_) moq_lite_session_close(self->session_);
        return;
    }
    self->HandleConnected();
}

void ClusterPushLink::HandleConnected() {
    connecting_ = false;
    bc_ = moq_lite_publish(session_, wire_.c_str());
    if (!bc_) {
        LOG_ERROR("[cluster] push %s: publish broadcast \"%s\" failed",
                  local_key_.c_str(), wire_.c_str());
        if (session_) moq_lite_session_close(session_);
        return;
    }

    PublishRenditions();
    linked_ = true;
    attempt_ = 0;
    LOG_INFO("[cluster] push %s linked: broadcast \"%s\", catalog %zu bytes",
             local_key_.c_str(), wire_.c_str(), catalog_json_.size());

    if (!paused_) AttachWriter();
}

void ClusterPushLink::PublishRenditions() {
    if (!session_ || !bc_) return;

    for (const auto &item : catalog_.renditions) {
        const MoqCatalogRendition &r = item.second;
        if (!r.usable) continue;
        if (track_by_name_.find(r.name) != track_by_name_.end()) continue;

        moq_lite_rendition_t rendition;
        rendition.name = r.name.c_str();
        rendition.codec = r.codec.c_str();
        rendition.config = r.config.empty() ? nullptr : r.config.data();
        rendition.config_len = r.config.size();
        rendition.width = r.width;
        rendition.height = r.height;
        rendition.sample_rate = r.sample_rate;
        rendition.channels = r.channels;
        rendition.bitrate = r.bitrate;

        const int is_video = (r.kind == "video") ? 1 : 0;
        const moq_lite_track_t track =
            moq_lite_publish_track(bc_, is_video, &rendition);
        if (!track) {
            LOG_ERROR("[cluster] push %s: publish track \"%s\" failed",
                      local_key_.c_str(), r.name.c_str());
            continue;
        }
        track_by_name_[r.name] = track;
        if (is_video && !video_track_) video_track_ = track;
        if (!is_video && !audio_track_) audio_track_ = track;
        LOG_INFO("[cluster] push %s: track \"%s\" (%s) published, config=%zu B",
                 local_key_.c_str(), r.name.c_str(), r.codec.c_str(),
                 r.config.size());
    }
}

void ClusterPushLink::AttachWriter() {
    if (!writer_ || writer_added_ || !linked_) return;
    writer_->SetInitFlag(false);
    MediaStreamManager::AddPlayer(writer_);
    writer_added_ = true;
    LOG_INFO("[cluster] push %s: frame writer attached", local_key_.c_str());
}

void ClusterPushLink::DetachWriter() {
    if (!writer_ || !writer_added_) return;
    MediaStreamManager::RemovePlayer(writer_);
    writer_added_ = false;
}

int ClusterPushLink::WritePacket(Media_Packet_Ptr pkt) {
    if (!linked_ || !pkt) return 0;
    if (pkt->is_seq_hdr_) return 0;   /* 编码参数由 catalog 携带 */
    if (!pkt->buffer_ptr_ || pkt->buffer_ptr_->DataLen() == 0) return 0;
    if (pkt->av_type_ != MEDIA_VIDEO_TYPE && pkt->av_type_ != MEDIA_AUDIO_TYPE) {
        return 0;
    }

    const uint8_t *body =
        reinterpret_cast<const uint8_t *>(pkt->buffer_ptr_->Data());
    const size_t body_len = pkt->buffer_ptr_->DataLen();
    const int64_t dts_ms = pkt->dts_ > 0 ? pkt->dts_ : 0;
    const uint64_t ts_us = static_cast<uint64_t>(dts_ms) * 1000;

    FlvBodyView view;
    if (pkt->av_type_ == MEDIA_VIDEO_TYPE) {
        if (!SplitFlvVideoBody(body, body_len, &view) || view.seq) return 0;
        if (!video_track_) return 0;
        moq_lite_track_frame(video_track_, ts_us, view.payload, view.len,
                             view.key ? 1 : 0);
    } else {
        if (!SplitFlvAudioBody(body, body_len, &view) || view.seq) return 0;
        if (!audio_track_) return 0;
        moq_lite_track_frame(audio_track_, ts_us, view.payload, view.len, 0);
    }
    return 0;
}

void ClusterPushLink::Pause() {
    if (paused_) return;
    paused_ = true;
    paused_at_ms_ = ClusterNowMs();
    DetachWriter();
    LOG_INFO("[cluster] push %s: local publisher gone, connection kept for %llds",
             local_key_.c_str(), (long long)(kPauseGraceMs / 1000));
}

void ClusterPushLink::Resume(const std::string &catalog_json) {
    paused_ = false;
    paused_at_ms_ = 0;
    if (catalog_json != catalog_json_) {
        catalog_json_ = catalog_json;
        ParseMoqCatalog(catalog_json_, &catalog_);
    }
    if (linked_) {
        PublishRenditions();
        AttachWriter();
    }
}

void ClusterPushLink::Stop() {
    if (stopped_) return;
    stopped_ = true;
    DetachWriter();
    if (session_) {
        moq_lite_session_close(session_);
    }
    linked_ = false;
    connecting_ = false;
    LOG_INFO("[cluster] push %s stopped", local_key_.c_str());
}

void ClusterPushLink::OnSessionClosed(moq_lite_session_t s, int err,
                                      const char *reason, void *user) {
    (void)s;
    ClusterPushLink *self = static_cast<ClusterPushLink *>(user);
    if (!self) return;
    LOG_WARN("[cluster] push %s: link closed (err=%d reason=%s)",
             self->local_key_.c_str(), err, reason ? reason : "");
    self->HandleClosed(err);
}

void ClusterPushLink::HandleClosed(int err) {
    (void)err;
    linked_ = false;
    connecting_ = false;
    session_dead_ = true;
    DetachWriter();
    close_pending_ = true;
    free_at_ms_ = ClusterNowMs() + kFreeDelayMs;
    bc_ = 0;
    track_by_name_.clear();
    video_track_ = audio_track_ = 0;
}

void ClusterPushLink::FreeSessionIfClosed(int64_t now_ms) {
    if (!close_pending_ || !session_ || !session_dead_) return;
    if (now_ms < free_at_ms_) return;

    moq_lite_session_free(session_);
    session_ = 0;
    session_dead_ = false;
    close_pending_ = false;
    bc_ = 0;
    track_by_name_.clear();
    video_track_ = audio_track_ = 0;

    if (!stopped_) {
        attempt_++;
        const int64_t delay =
            ClusterBackoffMs(opts_, attempt_, local_key_, now_ms);
        retry_at_ms_ = now_ms + delay;
        LOG_INFO("[cluster] push %s: reconnect in %lldms", local_key_.c_str(),
                 (long long)delay);
    }
}

void ClusterPushLink::OnTick(int64_t now_ms) {
    FreeSessionIfClosed(now_ms);

    if (!stopped_ && paused_ && paused_at_ms_ > 0 &&
        now_ms - paused_at_ms_ > kPauseGraceMs) {
        Stop();
    }

    if (!stopped_ && !session_ && !connecting_ && now_ms >= retry_at_ms_) {
        CreateSession();
    }
}

} /* namespace cpp_streamer */
