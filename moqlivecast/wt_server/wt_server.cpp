#include "wt_server/wt_server.hpp"
#include "logger.h"

#include <cstring>

namespace cpp_streamer {

WTServer::WTServer(uv_loop_t *loop) : loop_(loop) {
    if (!loop_) {
        LOG_ERROR("[wt-server] create: loop is null");
        return;
    }
    server_ = wt_server_create(loop_);
    if (!server_) {
        LOG_ERROR("[wt-server] wt_server_create failed");
    }
}

WTServer::~WTServer() {
    Stop();
}

int WTServer::AddPath(const std::string &path,
                      OnSession on_session,
                      OnStreamData on_stream_data,
                      OnSessionClose on_session_close) {
    if (!server_ || path.empty()) {
        LOG_ERROR("[wt-server] AddPath: invalid args");
        return -1;
    }
    if (listening_) {
        LOG_ERROR("[wt-server] AddPath: already listening");
        return -1;
    }

    auto ctx = std::make_unique<PathCbCtx>();
    ctx->owner = this;
    ctx->path = path;
    ctx->on_session = std::move(on_session);
    ctx->on_stream_data = std::move(on_stream_data);
    ctx->on_session_close = std::move(on_session_close);

    wt_path_callbacks_t cb;
    std::memset(&cb, 0, sizeof(cb));
    cb.on_session = OnSessionTrampoline;
    cb.on_stream_data = OnStreamDataTrampoline;
    cb.on_session_close = OnSessionCloseTrampoline;
    cb.user_data = ctx.get();

    wt_server_add_path(server_, path.c_str(), &cb);
    path_ctxs_.push_back(std::move(ctx));
    return 0;
}

void WTServer::SetOnOtherAlpn(OnOtherAlpn cb) {
    if (!server_) {
        LOG_ERROR("[wt-server] SetOnOtherAlpn: server not created");
        return;
    }
    on_other_alpn_ = std::move(cb);
    /* 包一层静态 trampoline —— C 侧只认函数指针。
     * 注册发生在 listen 前后都行：wt_server_listen_ex 会把已注册的回调搬到
     * 新建的 http3_server_api 上。 */
    wt_server_set_on_other_alpn(server_, OnOtherAlpnTrampoline, this);
}

void WTServer::OnOtherAlpnTrampoline(void *user_data, QuicConnection *conn,
                                     const char *alpn,
                                     const char *remote_ip, int remote_port) {
    auto *self = static_cast<WTServer *>(user_data);
    if (!self || !self->on_other_alpn_ || !conn) return;
    self->on_other_alpn_(conn,
                         alpn ? alpn : "",
                         remote_ip ? remote_ip : "",
                         remote_port);
}

bool WTServer::Start(const char *cert_file, const char *key_file,
                     const char *ip, int port) {
    return Start(cert_file, key_file, ip, port, nullptr, 0);
}

bool WTServer::Start(const char *cert_file, const char *key_file,
                     const char *ip, int port,
                     const char *const *alpn, int alpn_count) {
    if (!server_) {
        LOG_ERROR("[wt-server] Start: server not created");
        return false;
    }
    if (listening_) {
        LOG_ERROR("[wt-server] Start: already listening");
        return false;
    }
    if (!cert_file || !key_file || !ip || port <= 0) {
        LOG_ERROR("[wt-server] Start: invalid listen args");
        return false;
    }
    if (alpn && alpn_count > 0 && alpn_count > WT_SERVER_MAX_ALPN) {
        LOG_ERROR("[wt-server] Start: too many alpn entries (%d > %d)",
                  alpn_count, WT_SERVER_MAX_ALPN);
        return false;
    }

    if (wt_server_listen_ex(server_, cert_file, key_file, ip, port,
                            alpn, alpn_count) < 0) {
        LOG_ERROR("[wt-server] listen failed on %s:%d", ip, port);
        return false;
    }
    listening_ = true;

    /* 只 offer 一种协议时是裸 QUIC 这类单绑定端口；多种就是同端口多绑定 */
    std::string alpn_str;
    for (int i = 0; i < alpn_count; i++) {
        if (i) alpn_str += ",";
        alpn_str += alpn[i];
    }
    LOG_INFO("[wt-server] listening on %s:%d alpn=[%s]", ip, port,
             alpn_str.empty() ? "default: h3,quic-echo" : alpn_str.c_str());
    return true;
}

void WTServer::Stop() {
    if (!server_) return;
    sessions_.clear();
    wt_server_destroy(server_);
    server_ = nullptr;
    listening_ = false;
    path_ctxs_.clear();
    LOG_INFO("[wt-server] stopped");
}

WTServerSession *WTServer::EnsureSession(wt_session_t *sess,
                                         const std::string &path) {
    if (!sess) return nullptr;
    auto it = sessions_.find(sess);
    if (it != sessions_.end()) return it->second.get();
    auto wrapper = std::make_unique<WTServerSession>(sess, path);
    WTServerSession *p = wrapper.get();
    sessions_[sess] = std::move(wrapper);
    return p;
}

void WTServer::RemoveSession(wt_session_t *sess) {
    auto it = sessions_.find(sess);
    if (it == sessions_.end()) return;
    it->second->Detach();
    sessions_.erase(it);
}

void WTServer::OnSessionTrampoline(wt_server_t * /*srv*/, wt_session_t *sess,
                                   void *user) {
    auto *ctx = static_cast<PathCbCtx *>(user);
    if (!ctx || !ctx->owner) return;
    WTServerSession *wsess = ctx->owner->EnsureSession(sess, ctx->path);
    if (wsess && ctx->on_session) ctx->on_session(*wsess, ctx->path);
}

void WTServer::OnStreamDataTrampoline(wt_server_t * /*srv*/, wt_session_t *sess,
                                      wt_stream_t *st,
                                      const uint8_t *data, size_t len,
                                      void *user) {
    auto *ctx = static_cast<PathCbCtx *>(user);
    if (!ctx || !ctx->owner) return;
    WTServerSession *wsess = ctx->owner->EnsureSession(sess, ctx->path);
    if (!wsess) return;
    WTServerStream *stream = wsess->EnsureStream(st);
    if (!stream) return;
    stream->Feed(data, len);
    if (ctx->on_stream_data)
        ctx->on_stream_data(*wsess, *stream, data, len, ctx->path);
}

void WTServer::OnSessionCloseTrampoline(wt_server_t * /*srv*/,
                                        wt_session_t *sess, void *user) {
    auto *ctx = static_cast<PathCbCtx *>(user);
    if (!ctx || !ctx->owner) return;
    auto it = ctx->owner->sessions_.find(sess);
    if (it != ctx->owner->sessions_.end() && ctx->on_session_close) {
        ctx->on_session_close(*it->second, ctx->path);
    }
    ctx->owner->RemoveSession(sess);
}

} /* namespace cpp_streamer */
