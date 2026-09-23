#include "wt_server/wt_server_session.hpp"
#include "utils/meta_log.hpp"
#include "logger.h"

#include <cstring>

namespace cpp_streamer {

namespace {

class SessionMetaStats : public TimerInterface {
public:
    SessionMetaStats(WTServerSession *sess, std::string ev, std::string phase)
        : TimerInterface(static_cast<uint32_t>(MetaLog::Instance().IntervalMs()))
        , sess_(sess)
        , ev_(std::move(ev))
        , phase_(std::move(phase))
        , push_(phase_.find("push") != std::string::npos) {
        StartTimer();
    }

    ~SessionMetaStats() override { StopTimer(); }

    bool OnTimer() override {
        if (!sess_ || !sess_->Valid()) return false;
        QuicConnectionStats st;
        if (!sess_->GetQuicStats(&st)) return true;
        MetaLog::Instance().Stats(ev_, phase_, sess_->App(), sess_->Stream(),
                                  sess_->ExtraParams(), st, push_);
        return true;
    }

private:
    WTServerSession *sess_;
    std::string ev_;
    std::string phase_;
    bool push_;
};

} /* namespace */

WTServerSession::WTServerSession(wt_session_t *sess, std::string route_path)
    : sess_(sess)
    , route_path_(std::move(route_path)) {
    if (sess_) {
        wt_session_set_user_data(sess_, this);
        method_ = GetParam("method");
        app_ = GetParam("app");
        stream_ = GetParam("stream");
        session_id_ = std::to_string(wt_session_get_id(sess_));
        media_path_ = app_ + "/" + stream_;
    }
}

WTServerSession::~WTServerSession() {
    Detach();
}

std::string WTServerSession::Path() const {
    if (!sess_) return {};
    const char *p = wt_session_get_path(sess_);
    return p ? std::string(p) : std::string();
}

std::string WTServerSession::GetParam(const std::string &name) const {
    if (!sess_ || name.empty()) return {};
    const char *v = wt_session_get_param(sess_, name.c_str());
    return v ? std::string(v) : std::string();
}

std::string WTServerSession::ExtraParams() const {
    if (!sess_) return {};
    std::string out;
    const int n = wt_session_param_count(sess_);
    for (int i = 0; i < n; i++) {
        const char *k = nullptr;
        const char *v = nullptr;
        if (wt_session_param_at(sess_, i, &k, &v) != 0 || !k) continue;
        if (std::strcmp(k, "method") == 0 ||
            std::strcmp(k, "app") == 0 ||
            std::strcmp(k, "stream") == 0) {
            continue;
        }
        if (!out.empty()) out += "&";
        out += k;
        out += "=";
        if (v) out += v;
    }
    return out;
}

void WTServerSession::Close() {
    if (!sess_) return;
    wt_session_close(sess_);
}

bool WTServerSession::GetQuicStats(QuicConnectionStats *out) const {
    if (!sess_ || !out) return false;
    return wt_session_get_quic_stats(sess_, out) == 0;
}

void WTServerSession::StartMetaStats(const std::string &stats_ev,
                                     const std::string &phase) {
    StopMetaStats();
    if (!Valid() || !MetaLog::Instance().Enabled()) return;
    meta_stats_ = std::unique_ptr<TimerInterface>(
        new SessionMetaStats(this, stats_ev, phase));
}

void WTServerSession::StopMetaStats() {
    meta_stats_.reset();
}

void WTServerSession::OpenStream(OnStreamOpen cb) {
    if (!sess_) {
        LOG_ERROR("[wt-session] OpenStream: session invalid");
        return;
    }
    pending_open_cb_ = std::move(cb);
    wt_session_open_stream(sess_, OnStreamOpenTrampoline, this);
}

WTServerStream *WTServerSession::OpenUniStream() {
    if (!sess_) {
        LOG_ERROR("[wt-session] OpenUniStream: session invalid");
        return nullptr;
    }
    /* 与 OpenStream 不同，单向流创建后即可写，不需要回调通知 ——
     * 直接包成 WTServerStream 返回。 */
    wt_stream_t *st = wt_server_open_uni_stream(sess_);
    if (!st) return nullptr;
    return EnsureStream(st);
}

WTServerStream *WTServerSession::EnsureStream(wt_stream_t *st) {
    if (!st) return nullptr;
    auto it = streams_.find(st);
    if (it != streams_.end()) return it->second.get();
    auto wrapper = std::make_unique<WTServerStream>(st, this);
    WTServerStream *p = wrapper.get();
    streams_[st] = std::move(wrapper);
    return p;
}

void WTServerSession::RemoveStream(wt_stream_t *st) {
    auto it = streams_.find(st);
    if (it == streams_.end()) return;
    it->second->Detach();
    streams_.erase(it);
}

void WTServerSession::ForEachStream(const std::function<void(WTServerStream &st)> &fn) {
    if (!fn) return;
    for (auto &kv : streams_) {
        if (kv.second) fn(*kv.second);
    }
}

void WTServerSession::Detach() {
    StopMetaStats();
    for (auto &kv : streams_) {
        kv.second->Detach();
    }
    streams_.clear();
    if (sess_ && wt_session_get_user_data(sess_) == this) {
        wt_session_set_user_data(sess_, nullptr);
    }
    sess_ = nullptr;
}

WTServerSession *WTServerSession::From(wt_session_t *sess) {
    if (!sess) return nullptr;
    return static_cast<WTServerSession *>(wt_session_get_user_data(sess));
}

void WTServerSession::OnStreamOpenTrampoline(wt_server_t * /*srv*/,
                                             wt_session_t * /*sess*/,
                                             wt_stream_t *st, void *user) {
    auto *self = static_cast<WTServerSession *>(user);
    if (!self) return;
    WTServerStream *stream = self->EnsureStream(st);
    OnStreamOpen cb = std::move(self->pending_open_cb_);
    self->pending_open_cb_ = nullptr;
    if (cb && stream) cb(*self, *stream);
}

} /* namespace cpp_streamer */
