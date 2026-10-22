#include "cluster_pull.hpp"

#include "flv_tag_body.hpp"
#include "logger.h"
#include "media_stream_manager.hpp"

namespace cpp_streamer {

namespace {
constexpr int64_t kFreeDelayMs = 100;
const char *const kCatalogTrack = "catalog.json";
} /* namespace */

ClusterPullLink::ClusterPullLink(uv_loop_t *loop, const ClusterOptions &opts,
                                 std::string local_key, std::string wire,
                                 std::string owner_id)
    : loop_(loop),
      opts_(opts),
      local_key_(std::move(local_key)),
      wire_(std::move(wire)),
      owner_id_(std::move(owner_id)) {
    SplitLocalKey(local_key_, &app_, &stream_);
    const int64_t now = ClusterNowMs();
    last_demand_ms_ = now;
    retry_at_ms_ = now;
}

ClusterPullLink::~ClusterPullLink() {
    if (session_) {
        moq_lite_session_close(session_);
        session_ = 0;
    }
    if (claimed_) {
        MediaStreamManager::ReleasePublisher(local_key_, owner_id_);
        claimed_ = false;
    }
}

void ClusterPullLink::Start() {
    if (!stopped_ && session_) return;
    stopped_ = false;
    acquire_timed_out_ = false;
    /* 新的需求周期：重新给一整个 pull_acquire_timeout_ms 的预算。 */
    acquire_deadline_set_ = false;
    if (!session_ && !connecting_) CreateSession();
}

void ClusterPullLink::TouchDemand(int64_t now_ms) {
    last_demand_ms_ = now_ms;
}

void ClusterPullLink::CreateSession() {
    if (session_ || stopped_) return;

    const int64_t now = ClusterNowMs();
    if (!acquire_deadline_set_) {
        started_at_ms_ = now;
        acquire_deadline_ms_ = now + opts_.pull_acquire_timeout_ms;
        acquire_deadline_set_ = true;
    }

    session_ = moq_lite_session_new(loop_);
    if (!session_) {
        LOG_ERROR("[cluster] pull %s: cannot create moq-lite session",
                  local_key_.c_str());
        retry_at_ms_ = now + ClusterBackoffMs(opts_, attempt_, local_key_, now);
        return;
    }
    moq_lite_session_set_role(session_, MOQ_LITE_ROLE_SUBSCRIBER);

    moq_lite_session_cb_t cb;
    cb.on_connected = &ClusterPullLink::OnSessionConnected;
    cb.on_announce = nullptr;
    cb.on_announce_end = nullptr;
    cb.on_close = &ClusterPullLink::OnSessionClosed;
    cb.user = this;

    const std::string url = CenterUrl(opts_);
    connecting_ = true;
    if (moq_lite_session_connect(session_, url.c_str(), &cb) < 0) {
        LOG_ERROR("[cluster] pull %s: connect %s failed",
                  local_key_.c_str(), url.c_str());
        moq_lite_session_close(session_);
        connecting_ = false;
        close_pending_ = true;
        free_at_ms_ = now + kFreeDelayMs;
        retry_at_ms_ = now + ClusterBackoffMs(opts_, attempt_, local_key_, now);
        return;
    }
    LOG_INFO("[cluster] pull %s -> %s (broadcast \"%s\") connecting",
             local_key_.c_str(), url.c_str(), wire_.c_str());
}

void ClusterPullLink::OnSessionConnected(moq_lite_session_t s, int status,
                                         void *user) {
    (void)s;
    ClusterPullLink *self = static_cast<ClusterPullLink *>(user);
    if (!self || self->stopped_) return;
    if (status != 0) {
        LOG_WARN("[cluster] pull %s: connect failed (status=%d), will retry",
                 self->local_key_.c_str(), status);
        self->connecting_ = false;
        if (self->session_) moq_lite_session_close(self->session_);
        return;
    }
    self->HandleConnected();
}

void ClusterPullLink::HandleConnected() {
    connecting_ = false;
    attempt_ = 0;
    bc_ = moq_lite_broadcast_path(session_, wire_.c_str());
    if (!bc_) {
        LOG_ERROR("[cluster] pull %s: cannot create broadcast handle \"%s\"",
                  local_key_.c_str(), wire_.c_str());
        moq_lite_session_close(session_);
        return;
    }

    moq_lite_sub_cb_t cb;
    cb.on_subscribe = &ClusterPullLink::OnSubscribed;
    cb.on_frame = &ClusterPullLink::OnFrame;
    cb.on_track_info = nullptr;
    cb.on_subscribe_end = &ClusterPullLink::OnSubscribeEnd;
    cb.on_drop = &ClusterPullLink::OnDrop;
    cb.on_group_end = &ClusterPullLink::OnGroupEnd;
    cb.user = this;

    const moq_lite_sub_t sub =
        moq_lite_subscribe(bc_, kCatalogTrack, &cb);
    if (!sub) {
        LOG_ERROR("[cluster] pull %s: subscribe catalog failed: %s",
                  local_key_.c_str(), moq_lite_strerror(-1));
        moq_lite_session_close(session_);
        return;
    }
    sub_track_[sub] = kCatalogTrack;
    linked_ = true;
    LOG_INFO("[cluster] pull %s linked: subscribing catalog on \"%s\"",
             local_key_.c_str(), wire_.c_str());
}

void ClusterPullLink::OnSessionClosed(moq_lite_session_t s, int err,
                                      const char *reason, void *user) {
    (void)s;
    ClusterPullLink *self = static_cast<ClusterPullLink *>(user);
    if (!self) return;
    LOG_WARN("[cluster] pull %s: link closed (err=%d reason=%s)",
             self->local_key_.c_str(), err, reason ? reason : "");
    self->HandleClosed(err);
}

void ClusterPullLink::HandleClosed(int err) {
    (void)err;
    linked_ = false;
    connecting_ = false;
    session_dead_ = true;
    close_pending_ = true;
    free_at_ms_ = ClusterNowMs() + kFreeDelayMs;
    ClearSessionHandles();
}

void ClusterPullLink::ClearSessionHandles() {
    bc_ = 0;
    sub_track_.clear();
    track_sub_.clear();
    /* 重连后组号从 SUBSCRIBE_START 重新开始，重排/时间轴基准一并清掉；
     * 新的绝对时间戳会经 MediaStreamManager::NormalizeDts 重新接上本地
     * 时间轴（它按「输入 dts 回退 >1s」识别并平移）。 */
    track_state_.clear();
    last_dts_ms_.clear();
    base_ms_ = -1;
}

void ClusterPullLink::FreeSessionIfClosed(int64_t now_ms) {
    if (!session_) return;

    /* 回调里请求的重连：等这一拍结束再关，避免在 C 回调里动会话。 */
    if (reconnect_requested_ && !close_pending_) {
        reconnect_requested_ = false;
        moq_lite_session_close(session_);
        close_pending_ = true;
    }

    if (!close_pending_ || !session_dead_ || now_ms < free_at_ms_) return;

    moq_lite_session_free(session_);
    session_ = 0;
    session_dead_ = false;
    close_pending_ = false;
    ClearSessionHandles();

    if (!stopped_) {
        attempt_++;
        const int64_t delay =
            ClusterBackoffMs(opts_, attempt_, local_key_, now_ms);
        retry_at_ms_ = now_ms + delay;
        LOG_INFO("[cluster] pull %s: reconnect in %lldms", local_key_.c_str(),
                 (long long)delay);
    }
}

void ClusterPullLink::OnTick(int64_t now_ms) {
    FreeSessionIfClosed(now_ms);

    if (!stopped_ && !session_ && !connecting_ && now_ms >= retry_at_ms_) {
        CreateSession();
    }

    /* 重排队列里等一个始终不来的组：对端丢组/被 reset 时不能把后面的组
     * 全堵死。2s 后跳到最小的已到达组。 */
    for (auto &kv : track_state_) {
        PullTrackState &st = kv.second;
        if (st.pending.empty()) continue;
        const uint64_t front = st.pending.begin()->first;
        if (front <= st.next_group) continue;
        if (st.oldest_pending_ms == 0) {
            st.oldest_pending_ms = now_ms;
            continue;
        }
        if (now_ms - st.oldest_pending_ms <= 2000) continue;
        LOG_WARN("[cluster] pull %s: track \"%s\" group %llu missing, skipping "
                 "to group %llu", local_key_.c_str(), kv.first.c_str(),
                 static_cast<unsigned long long>(st.next_group),
                 static_cast<unsigned long long>(front));
        st.next_group = front;
        st.oldest_pending_ms = now_ms;
        DrainPending(kv.first);
    }

    /* 连上了但一直拿不到 catalog：中心没有这条流，放弃并写负缓存。 */
    if (!stopped_ && !claimed_ && acquire_deadline_ms_ > 0 &&
        now_ms >= acquire_deadline_ms_) {
        LOG_INFO("[cluster] pull %s: no catalog within %dms, giving up",
                 local_key_.c_str(), opts_.pull_acquire_timeout_ms);
        acquire_timed_out_ = true;
        Stop();
    }

    /* 拿到了 catalog 但长时间没有媒体：中心那边停了，关闭连接；本地
     * 播放器还在的话，下一次播放请求会重新触发回源。 */
    if (!stopped_ && claimed_ && last_media_ms_ > 0 &&
        now_ms - last_media_ms_ > opts_.pull_idle_timeout_ms) {
        LOG_INFO("[cluster] pull %s: no media for %dms, closing",
                 local_key_.c_str(), opts_.pull_idle_timeout_ms);
        Stop();
    }
}

void ClusterPullLink::Stop() {
    if (stopped_) return;
    stopped_ = true;
    if (claimed_) {
        claimed_ = false;
        /* 清本地 catalog / GOP；播放器仍在 writer_map_ 里等着，下一条推流
         * 或下一次回源会把时间轴平移后继续。 */
        MediaStreamManager::ReleasePublisher(local_key_, owner_id_);
    }
    if (session_) {
        moq_lite_session_close(session_);
    }
    linked_ = false;
    connecting_ = false;
    LOG_INFO("[cluster] pull %s stopped", local_key_.c_str());
}

void ClusterPullLink::AbandonLocalWins() {
    LOG_INFO("[cluster] pull %s: local publisher owns this key, giving up pull",
             local_key_.c_str());
    stopped_ = true;
    if (session_) moq_lite_session_close(session_);
    linked_ = false;
    connecting_ = false;
}

void ClusterPullLink::OnSubscribed(moq_lite_sub_t sub, uint64_t first_group,
                                   void *user) {
    ClusterPullLink *self = static_cast<ClusterPullLink *>(user);
    if (!self) return;
    auto it = self->sub_track_.find(sub);
    const std::string name =
        it == self->sub_track_.end() ? std::string() : it->second;
    LOG_INFO("[cluster] pull %s: subscribe %s started at group %llu",
             self->local_key_.c_str(), name.empty() ? "?" : name.c_str(),
             static_cast<unsigned long long>(first_group));
    if (!name.empty() && name != kCatalogTrack) {
        PullTrackState &st = self->track_state_[name];
        st.next_group = first_group;
        st.started = true;
    }
}

void ClusterPullLink::OnFrame(moq_lite_sub_t sub, uint64_t group,
                              uint64_t frame_index, int64_t ts_delta,
                              const uint8_t *payload, size_t len, void *user) {
    (void)group;
    (void)ts_delta;
    ClusterPullLink *self = static_cast<ClusterPullLink *>(user);
    if (!self || self->stopped_) return;

    auto it = self->sub_track_.find(sub);
    if (it == self->sub_track_.end()) return;

    if (it->second == kCatalogTrack) {
        self->HandleCatalogFrame(payload, len);
    } else {
        self->HandleMediaFrame(sub, group, frame_index, payload, len);
    }
}

void ClusterPullLink::OnSubscribeEnd(moq_lite_sub_t sub, uint64_t end_group,
                                     void *user) {
    (void)sub;
    (void)end_group;
    ClusterPullLink *self = static_cast<ClusterPullLink *>(user);
    if (!self || self->stopped_) return;
    /* 对端结束了某条轨：重新连一次，catalog 会把新状态带回来。 */
    self->reconnect_requested_ = true;
}

void ClusterPullLink::OnDrop(moq_lite_sub_t sub, uint64_t gstart, uint64_t gend,
                             uint64_t error_code, void *user) {
    (void)gstart;
    (void)gend;
    ClusterPullLink *self = static_cast<ClusterPullLink *>(user);
    if (!self || self->stopped_) return;
    auto it = self->sub_track_.find(sub);
    LOG_INFO("[cluster] pull %s: subscribe %s dropped (code=0x%llx)",
             self->local_key_.c_str(),
             it == self->sub_track_.end() ? "?" : it->second.c_str(),
             static_cast<unsigned long long>(error_code));
    self->reconnect_requested_ = true;
}

void ClusterPullLink::HandleCatalogFrame(const uint8_t *payload, size_t len) {
    if (!payload || len == 0) return;

    /* catalog 是数据轨：没有容器时间戳前缀，载荷就是裸 JSON。 */
    const std::string raw(reinterpret_cast<const char *>(payload), len);
    MoqCatalogInfo info;
    if (!ParseMoqCatalog(raw, &info)) {
        LOG_WARN("[cluster] pull %s: catalog is not valid JSON (%zu bytes)",
                 local_key_.c_str(), len);
        return;
    }

    /* 本地推流优先：同 key 已经有别的 publisher 就放弃回源。 */
    if (MediaStreamManager::HasPublisher(local_key_) &&
        !MediaStreamManager::IsPublisherOwner(local_key_, owner_id_)) {
        AbandonLocalWins();
        return;
    }

    if (!claimed_) {
        const PublishClaimResult claim =
            MediaStreamManager::ClaimPublisher(local_key_, owner_id_);
        if (claim.action == PublishClaim::kTakeOver ||
            claim.action == PublishClaim::kRejected) {
            AbandonLocalWins();
            return;
        }
        claimed_ = true;
        LOG_INFO("[cluster] pull %s: claimed local publisher (owner=%s)",
                 local_key_.c_str(), owner_id_.c_str());
    }

    catalog_json_ = raw;
    catalog_ = info;
    /* 幂等：内容没变时不会触发下游补组。 */
    MediaStreamManager::SetCatalogJson(local_key_, owner_id_, raw);

    for (const auto &item : catalog_.renditions) {
        const MoqCatalogRendition &r = item.second;
        if (!r.usable) continue;
        SendSequenceHeader(r.name);
        SubscribeTrack(r.name);
    }
}

void ClusterPullLink::SendSequenceHeader(const std::string &name) {
    auto it = catalog_.renditions.find(name);
    if (it == catalog_.renditions.end()) return;
    const MoqCatalogRendition &r = it->second;
    if (!r.usable || r.config.empty()) return;

    const std::string sig = r.codec + ":" + std::to_string(r.config.size());
    if (seq_hdr_sig_[name] == sig) return;

    Media_Packet_Ptr pkt;
    if (r.kind == "video") {
        pkt = MakeFlvVideoPacket(0, /*seq=*/true, /*key=*/false,
                                 r.config.data(), r.config.size());
    } else {
        pkt = MakeFlvAudioPacket(0, /*seq=*/true, r.config.data(), r.config.size());
        pkt->sample_rate_ = r.sample_rate;
        pkt->channel_ = static_cast<uint8_t>(r.channels & 0xff);
    }
    pkt->app_ = app_;
    pkt->streamname_ = stream_;
    pkt->key_ = local_key_;

    MediaStreamManager::WriterMediaPacket(pkt);
    seq_hdr_sig_[name] = sig;
    LOG_INFO("[cluster] pull %s: sent FLV sequence header for track \"%s\" "
             "(%zu bytes)", local_key_.c_str(), name.c_str(), r.config.size());
}

void ClusterPullLink::SubscribeTrack(const std::string &name) {
    if (!bc_ || track_sub_.count(name)) return;

    moq_lite_sub_cb_t cb;
    cb.on_subscribe = &ClusterPullLink::OnSubscribed;
    cb.on_frame = &ClusterPullLink::OnFrame;
    cb.on_track_info = nullptr;
    cb.on_subscribe_end = &ClusterPullLink::OnSubscribeEnd;
    cb.on_drop = &ClusterPullLink::OnDrop;
    cb.on_group_end = &ClusterPullLink::OnGroupEnd;
    cb.user = this;

    const moq_lite_sub_t sub = moq_lite_subscribe(bc_, name.c_str(), &cb);
    if (!sub) {
        LOG_WARN("[cluster] pull %s: subscribe track \"%s\" failed",
                 local_key_.c_str(), name.c_str());
        return;
    }
    track_sub_[name] = sub;
    sub_track_[sub] = name;
    LOG_INFO("[cluster] pull %s: subscribing track \"%s\"",
             local_key_.c_str(), name.c_str());
}

void ClusterPullLink::HandleMediaFrame(moq_lite_sub_t sub, uint64_t group,
                                       uint64_t frame_index,
                                       const uint8_t *payload, size_t len) {
    if (!claimed_) return;
    if (!payload || len == 0) return;
    if (!MediaStreamManager::IsPublisherOwner(local_key_, owner_id_)) {
        AbandonLocalWins();
        return;
    }

    auto sit = sub_track_.find(sub);
    if (sit == sub_track_.end()) return;
    auto rit = catalog_.renditions.find(sit->second);
    if (rit == catalog_.renditions.end()) return;
    const MoqCatalogRendition &r = rit->second;

    uint64_t ts_us = 0;
    const uint8_t *data = payload;
    size_t data_len = len;
    if (moq_lite_strip_container_ts(payload, len, &ts_us, &data, &data_len) < 0) {
        /* legacy 媒体帧必须带绝对微秒前缀；没有就没法还原时间轴。 */
        LOG_DEBUG("[cluster] pull %s: track \"%s\" frame without container ts",
                  local_key_.c_str(), r.name.c_str());
        return;
    }
    if (data_len == 0) return;

    const bool key = (r.kind == "video") && (frame_index == 0);
    HandleTrackFrame(r.name, group, ts_us, key, data, data_len);
}

void ClusterPullLink::HandleTrackFrame(const std::string &name, uint64_t group,
                                       uint64_t ts_us, bool key,
                                       const uint8_t *data, size_t len) {
    PullTrackState &st = track_state_[name];
    if (!st.started) {
        st.next_group = group;
        st.started = true;
    }
    if (group < st.next_group) return;   /* 已经越过的旧组 */

    if (group == st.next_group && st.pending.find(group) == st.pending.end()) {
        PullFrame frame;
        frame.ts_us = ts_us;
        frame.key = key;
        frame.data.assign(data, data + len);
        EmitFrame(name, frame);
        return;
    }

    if (st.pending.empty()) st.oldest_pending_ms = ClusterNowMs();
    PullFrame frame;
    frame.ts_us = ts_us;
    frame.key = key;
    frame.data.assign(data, data + len);
    st.pending[group].push_back(std::move(frame));
}

void ClusterPullLink::OnGroupEnd(moq_lite_sub_t sub, uint64_t group,
                                 void *user) {
    ClusterPullLink *self = static_cast<ClusterPullLink *>(user);
    if (!self || self->stopped_) return;
    auto it = self->sub_track_.find(sub);
    if (it == self->sub_track_.end() || it->second == kCatalogTrack) return;
    self->OnGroupEndInternal(it->second, group);
}

void ClusterPullLink::OnGroupEndInternal(const std::string &name,
                                         uint64_t group) {
    PullTrackState &st = track_state_[name];
    if (!st.started) {
        st.next_group = group;
        st.started = true;
    }
    if (group < st.next_group) return;

    if (group == st.next_group) {
        st.ended.erase(group);
        st.next_group++;
        DrainPending(name);
    } else {
        st.ended.insert(group);
    }
}

void ClusterPullLink::DrainPending(const std::string &name) {
    PullTrackState &st = track_state_[name];
    while (true) {
        auto it = st.pending.find(st.next_group);
        if (it == st.pending.end()) break;

        for (const PullFrame &frame : it->second) EmitFrame(name, frame);
        const bool ended = st.ended.erase(st.next_group) > 0;
        st.pending.erase(it);
        if (!ended) {
            /* 这一组还没收齐：已经发出的部分先走，后续帧由 HandleTrackFrame
             * 的即时路径直接发。 */
            break;
        }
        st.next_group++;
    }
    if (st.pending.empty()) {
        st.oldest_pending_ms = 0;
    } else if (st.oldest_pending_ms == 0) {
        st.oldest_pending_ms = ClusterNowMs();
    }
}

void ClusterPullLink::EmitFrame(const std::string &name,
                                const PullFrame &frame) {
    auto rit = catalog_.renditions.find(name);
    if (rit == catalog_.renditions.end()) return;
    const MoqCatalogRendition &r = rit->second;

    const int64_t ts_ms = static_cast<int64_t>(frame.ts_us / 1000);
    if (base_ms_ < 0) base_ms_ = ts_ms;
    int64_t dts_ms = ts_ms - base_ms_;
    if (dts_ms < 0) dts_ms = 0;

    /* 时间轴单调性守卫：同一轨的 dts 回退/重复会让下游 muxer 直接断开
     * （ffmpeg: "non monotonically increasing dts"）。回放边界上偶尔会重复
     * 一帧（GOP 末帧 == live 首帧）；legacy 线上没有 CTS，dts 就是唯一顺序，
     * 重复的直接丢掉。真正的回退（重连/时钟跳变）也走这里 —— 丢一帧总比
     * 让 RTMP/HTTP-FLV 播放器整条断开好。限流打一条 WARN 便于观察。 */
    auto lit = last_dts_ms_.find(name);
    if (lit != last_dts_ms_.end() && dts_ms <= lit->second) {
        if (ClusterNowMs() - dts_warn_ms_ >= 1000) {
            dts_warn_ms_ = ClusterNowMs();
            LOG_WARN("[cluster] pull %s: track \"%s\" dts not increasing: "
                     "ts_us=%llu base=%lld dts=%lld prev=%lld — frame dropped",
                     local_key_.c_str(), name.c_str(),
                     static_cast<unsigned long long>(frame.ts_us),
                     (long long)base_ms_, (long long)dts_ms,
                     (long long)lit->second);
        }
        return;
    }
    last_dts_ms_[name] = dts_ms;

    Media_Packet_Ptr pkt;
    if (r.kind == "video") {
        pkt = MakeFlvVideoPacket(dts_ms, /*seq=*/false, frame.key,
                                 frame.data.data(), frame.data.size());
    } else {
        pkt = MakeFlvAudioPacket(dts_ms, /*seq=*/false, frame.data.data(),
                                 frame.data.size());
        pkt->sample_rate_ = r.sample_rate;
        pkt->channel_ = static_cast<uint8_t>(r.channels & 0xff);
    }
    pkt->app_ = app_;
    pkt->streamname_ = stream_;
    pkt->key_ = local_key_;

    MediaStreamManager::WriterMediaPacket(pkt);
    last_media_ms_ = ClusterNowMs();
}

} /* namespace cpp_streamer */
