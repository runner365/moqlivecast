#include "cluster_discovery.hpp"

#include "logger.h"

namespace cpp_streamer {

namespace {
constexpr int64_t kFreeDelayMs = 100;
} /* namespace */

ClusterDiscovery::ClusterDiscovery(uv_loop_t *loop, const ClusterOptions &opts,
                                   Sink sink)
    : loop_(loop), opts_(opts), sink_(std::move(sink)) {
    retry_at_ms_ = ClusterNowMs();
}

ClusterDiscovery::~ClusterDiscovery() {
    if (session_) {
        moq_lite_session_close(session_);
        session_ = 0;
    }
}

void ClusterDiscovery::Start() {
    if (!stopped_ && session_) return;
    stopped_ = false;
    if (!session_ && !connecting_) CreateSession();
}

void ClusterDiscovery::CreateSession() {
    if (session_ || stopped_) return;

    const int64_t now = ClusterNowMs();
    session_ = moq_lite_session_new(loop_);
    if (!session_) {
        LOG_ERROR("[cluster] discovery: cannot create session");
        retry_at_ms_ = now + ClusterBackoffMs(opts_, attempt_, "discovery", now);
        return;
    }
    moq_lite_session_set_role(session_, MOQ_LITE_ROLE_SUBSCRIBER);

    moq_lite_session_cb_t cb;
    cb.on_connected = &ClusterDiscovery::OnConnected;
    cb.on_announce = &ClusterDiscovery::OnAnnounce;
    cb.on_announce_end = &ClusterDiscovery::OnAnnounceEnd;
    cb.on_close = &ClusterDiscovery::OnClosed;
    cb.user = this;

    const std::string url = CenterUrl(opts_);
    connecting_ = true;
    if (moq_lite_session_connect(session_, url.c_str(), &cb) < 0) {
        LOG_ERROR("[cluster] discovery: connect %s failed", url.c_str());
        moq_lite_session_close(session_);
        connecting_ = false;
        close_pending_ = true;
        free_at_ms_ = now + kFreeDelayMs;
        retry_at_ms_ = now + ClusterBackoffMs(opts_, attempt_, "discovery", now);
        return;
    }
    LOG_INFO("[cluster] discovery -> %s connecting", url.c_str());
}

void ClusterDiscovery::OnConnected(moq_lite_session_t s, int status, void *user) {
    (void)s;
    ClusterDiscovery *self = static_cast<ClusterDiscovery *>(user);
    if (!self || self->stopped_) return;
    if (status != 0) {
        LOG_WARN("[cluster] discovery: connect failed (status=%d), will retry",
                 status);
        self->connecting_ = false;
        if (self->session_) moq_lite_session_close(self->session_);
        return;
    }
    self->connecting_ = false;
    self->attempt_ = 0;
    if (moq_lite_session_announce(self->session_, "") < 0) {
        LOG_ERROR("[cluster] discovery: announce failed");
        moq_lite_session_close(self->session_);
        return;
    }
    LOG_INFO("[cluster] discovery linked: announce requested");
}

void ClusterDiscovery::OnClosed(moq_lite_session_t s, int err,
                                const char *reason, void *user) {
    (void)s;
    ClusterDiscovery *self = static_cast<ClusterDiscovery *>(user);
    if (!self) return;
    LOG_WARN("[cluster] discovery closed (err=%d reason=%s)", err,
             reason ? reason : "");
    self->HandleClosed(err);
}

void ClusterDiscovery::HandleClosed(int err) {
    (void)err;
    connecting_ = false;
    session_dead_ = true;
    close_pending_ = true;
    free_at_ms_ = ClusterNowMs() + kFreeDelayMs;
    ClearKnown(true);
    bc_paths_.clear();
}

void ClusterDiscovery::ClearKnown(bool notify) {
    if (notify && sink_) {
        for (const std::string &wire : known_) sink_(wire, false);
    }
    known_.clear();
}

void ClusterDiscovery::FreeSessionIfClosed(int64_t now_ms) {
    if (!session_ || !close_pending_ || !session_dead_ || now_ms < free_at_ms_) {
        return;
    }
    moq_lite_session_free(session_);
    session_ = 0;
    session_dead_ = false;
    close_pending_ = false;
    bc_paths_.clear();

    if (!stopped_) {
        attempt_++;
        const int64_t delay =
            ClusterBackoffMs(opts_, attempt_, "discovery", now_ms);
        retry_at_ms_ = now_ms + delay;
        LOG_INFO("[cluster] discovery: reconnect in %lldms", (long long)delay);
    }
}

void ClusterDiscovery::OnTick(int64_t now_ms) {
    FreeSessionIfClosed(now_ms);
    if (!stopped_ && !session_ && !connecting_ && now_ms >= retry_at_ms_) {
        CreateSession();
    }
}

void ClusterDiscovery::Stop() {
    if (stopped_) return;
    stopped_ = true;
    ClearKnown(true);
    if (session_) moq_lite_session_close(session_);
    connecting_ = false;
    LOG_INFO("[cluster] discovery stopped");
}

void ClusterDiscovery::OnAnnounce(moq_lite_session_t s, moq_lite_broadcast_t bc,
                                  const char *path, const char *suffix,
                                  void *user) {
    (void)s;
    (void)suffix;
    ClusterDiscovery *self = static_cast<ClusterDiscovery *>(user);
    if (!self || self->stopped_ || !path || !path[0]) return;

    self->bc_paths_[bc] = path;
    if (self->known_.insert(path).second) {
        LOG_INFO("[cluster] discovery: broadcast \"%s\" available", path);
        if (self->sink_) self->sink_(path, true);
    }
}

void ClusterDiscovery::OnAnnounceEnd(moq_lite_session_t s,
                                     moq_lite_broadcast_t bc, void *user) {
    (void)s;
    ClusterDiscovery *self = static_cast<ClusterDiscovery *>(user);
    if (!self || self->stopped_) return;

    auto it = self->bc_paths_.find(bc);
    if (it == self->bc_paths_.end()) return;
    const std::string path = it->second;
    self->bc_paths_.erase(it);
    if (self->known_.erase(path) > 0) {
        LOG_INFO("[cluster] discovery: broadcast \"%s\" gone", path.c_str());
        if (self->sink_) self->sink_(path, false);
    }
}

} /* namespace cpp_streamer */
