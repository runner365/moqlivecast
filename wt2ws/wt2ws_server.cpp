#include "wt2ws_server.hpp"

#include "logger.h"

#include <utility>

namespace cpp_streamer {
namespace wt2ws {

Wt2wsServer::Wt2wsServer(uv_loop_t *loop) : loop_(loop) {}

Wt2wsServer::~Wt2wsServer() {
    Stop();
}

bool Wt2wsServer::Start(const Wt2wsConfig &cfg, std::string *err) {
    cfg_ = cfg;
    srv_ = wt_server_create(loop_);
    if (!srv_) {
        *err = "wt_server_create failed";
        return false;
    }

    wt_path_callbacks_t cb;
    cb.on_session = &Wt2wsServer::OnSessionTramp;
    cb.on_stream_data = &Wt2wsServer::OnStreamTramp;
    cb.on_session_close = &Wt2wsServer::OnCloseTramp;
    cb.user_data = this;
    wt_server_add_path(srv_, cfg_.path.c_str(), &cb);

    if (wt_server_listen(srv_, cfg_.cert_file.c_str(), cfg_.key_file.c_str(),
                         cfg_.listen_ip.c_str(), cfg_.listen_port) < 0) {
        *err = "wt_server_listen failed (cert/key/listen address?)";
        wt_server_destroy(srv_);
        srv_ = nullptr;
        return false;
    }

    uv_timer_init(loop_, &idle_timer_);
    idle_timer_.data = this;
    uv_timer_start(&idle_timer_, &Wt2wsServer::OnIdleTimer, 1000, 1000);
    idle_timer_started_ = true;

    LOG_INFO("[wt2ws] listening wt://%s:%d%s -> ws://%s:%d%s",
             cfg_.listen_ip.c_str(), cfg_.listen_port, cfg_.path.c_str(),
             cfg_.backend_host.c_str(), cfg_.backend_port,
             cfg_.backend_path.c_str());
    return true;
}

void Wt2wsServer::Stop() {
    if (idle_timer_started_) {
        uv_timer_stop(&idle_timer_);
        idle_timer_started_ = false;
    }
    if (srv_) {
        wt_server_destroy(srv_);
        srv_ = nullptr;
    }
    sessions_.clear();
    retired_.clear();
}

void Wt2wsServer::OnSession(wt_session_t *sess) {
    if (!sess) return;
    const uint64_t id = wt_session_get_id(sess);
    if (static_cast<int>(sessions_.size()) >= cfg_.max_sessions) {
        LOG_WARN("[wt2ws] reject session %llu: max_sessions=%d",
                 static_cast<unsigned long long>(id), cfg_.max_sessions);
        wt_session_close(sess);
        return;
    }

    auto s = std::make_shared<Wt2wsSession>(loop_, cfg_, sess);
    sessions_[id] = s;
    LOG_INFO("[wt2ws] session %llu open path=%s",
             static_cast<unsigned long long>(id), wt_session_get_path(sess));
    s->Start();
}

void Wt2wsServer::OnStreamData(wt_session_t *sess, wt_stream_t *st,
                               const uint8_t *data, size_t len) {
    if (!sess || !st) return;
    const uint64_t id = wt_session_get_id(sess);
    auto it = sessions_.find(id);
    if (it == sessions_.end()) return;

    if (wt_stream_is_uni(st)) {
        /* v1: the relay uses one client-initiated bidirectional stream. */
        LOG_DEBUG("[wt2ws] session %llu ignores data on uni stream",
                  static_cast<unsigned long long>(id));
        return;
    }
    if (it->second->stream() && it->second->stream() != st) {
        LOG_WARN("[wt2ws] session %llu opened a second bidi stream, closing",
                 static_cast<unsigned long long>(id));
        wt_session_close(sess);
        return;
    }
    it->second->AttachStream(st);
    it->second->OnWtData(data, len);
}

void Wt2wsServer::OnSessionClose(wt_session_t *sess) {
    if (!sess) return;
    const uint64_t id = wt_session_get_id(sess);
    auto it = sessions_.find(id);
    if (it == sessions_.end()) return;
    auto s = it->second;
    s->OnWtClosed();
    retired_.push_back(s);
    sessions_.erase(it);
    LOG_INFO("[wt2ws] session %llu closed (active=%zu, retired=%zu)",
             static_cast<unsigned long long>(id), sessions_.size(),
             retired_.size());
}

void Wt2wsServer::OnTick() {
    const int64_t now = static_cast<int64_t>(uv_now(loop_));
    std::vector<std::shared_ptr<Wt2wsSession>> snapshot;
    snapshot.reserve(sessions_.size());
    for (auto &kv : sessions_) snapshot.push_back(kv.second);
    for (auto &s : snapshot) s->OnTick(now);
}

void Wt2wsServer::OnSessionTramp(wt_server_t *srv, wt_session_t *sess,
                                 void *user) {
    (void)srv;
    static_cast<Wt2wsServer *>(user)->OnSession(sess);
}

void Wt2wsServer::OnStreamTramp(wt_server_t *srv, wt_session_t *sess,
                                wt_stream_t *st, const uint8_t *data, size_t len,
                                void *user) {
    (void)srv;
    static_cast<Wt2wsServer *>(user)->OnStreamData(sess, st, data, len);
}

void Wt2wsServer::OnCloseTramp(wt_server_t *srv, wt_session_t *sess,
                               void *user) {
    (void)srv;
    static_cast<Wt2wsServer *>(user)->OnSessionClose(sess);
}

void Wt2wsServer::OnIdleTimer(uv_timer_t *timer) {
    static_cast<Wt2wsServer *>(timer->data)->OnTick();
}

} /* namespace wt2ws */
} /* namespace cpp_streamer */
