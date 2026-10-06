#ifndef WT_SERVER_HPP
#define WT_SERVER_HPP

#include "webtransport_server_api.h"
#include "wt_server/wt_server_session.hpp"

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include <uv.h>

namespace cpp_streamer {

class WTServer {
public:
    using OnSession = std::function<void(WTServerSession &sess,
                                         const std::string &path)>;
    using OnStreamData = std::function<void(WTServerSession &sess,
                                            WTServerStream &st,
                                            const uint8_t *data, size_t len,
                                            const std::string &path)>;
    using OnSessionClose = std::function<void(WTServerSession &sess,
                                              const std::string &path)>;
    /* 非 h3 的 ALPN 连接（同端口跑别的 QUIC 协议，如裸 QUIC 的 moq-lite）。
     * 握手完成时回调 —— ALPN 那时才定得下来。注册方自行接线。 */
    using OnOtherAlpn = std::function<void(QuicConnection *conn,
                                           const std::string &alpn,
                                           const std::string &remote_ip,
                                           int remote_port)>;

    explicit WTServer(uv_loop_t *loop);
    ~WTServer();

    WTServer(const WTServer &) = delete;
    WTServer &operator=(const WTServer &) = delete;

    int AddPath(const std::string &path,
                OnSession on_session,
                OnStreamData on_stream_data,
                OnSessionClose on_session_close = nullptr);

    /* listen 前后注册都可以。未注册时非 h3 的连接会被服务端直接关闭。 */
    void SetOnOtherAlpn(OnOtherAlpn cb);

    /* ALPN 候选表上限 —— 只是防呆，正常两三条 */
    static const int WT_SERVER_MAX_ALPN = 8;

    bool Start(const char *cert_file, const char *key_file,
               const char *ip, int port);
    /* 本端口接受的应用协议，服务端偏好优先，**必须在 Start 时一次给定**
     * （ALPN 在握手时协商）。alpn = nullptr → 默认列表 h3 + quic-echo。
     * 想让 OBS 的裸 QUIC 与浏览器的 WebTransport 共用这个端口，就往表里
     * 加 "moq-lite-06" —— 包按连接 ID 分，协议在握手时定一次。 */
    bool Start(const char *cert_file, const char *key_file,
               const char *ip, int port,
               const char *const *alpn, int alpn_count);
    void Stop();

    uv_loop_t *Loop() const { return loop_; }
    wt_server_t *Handle() const { return server_; }
    bool Listening() const { return listening_; }

private:
    struct PathCbCtx {
        WTServer *owner = nullptr;
        std::string path;
        OnSession on_session;
        OnStreamData on_stream_data;
        OnSessionClose on_session_close;
    };

    static void OnSessionTrampoline(wt_server_t *srv, wt_session_t *sess,
                                    void *user);
    static void OnStreamDataTrampoline(wt_server_t *srv, wt_session_t *sess,
                                       wt_stream_t *st,
                                       const uint8_t *data, size_t len,
                                       void *user);
    static void OnSessionCloseTrampoline(wt_server_t *srv, wt_session_t *sess,
                                         void *user);
    static void OnOtherAlpnTrampoline(void *user_data, QuicConnection *conn,
                                      const char *alpn,
                                      const char *remote_ip, int remote_port);

    WTServerSession *EnsureSession(wt_session_t *sess, const std::string &path);
    void RemoveSession(wt_session_t *sess);

    uv_loop_t *loop_ = nullptr;
    wt_server_t *server_ = nullptr;
    bool listening_ = false;
    std::vector<std::unique_ptr<PathCbCtx>> path_ctxs_;
    OnOtherAlpn on_other_alpn_;
    std::unordered_map<wt_session_t *, std::unique_ptr<WTServerSession>> sessions_;
};

} /* namespace cpp_streamer */

#endif /* WT_SERVER_HPP */
