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

    explicit WTServer(uv_loop_t *loop);
    ~WTServer();

    WTServer(const WTServer &) = delete;
    WTServer &operator=(const WTServer &) = delete;

    int AddPath(const std::string &path,
                OnSession on_session,
                OnStreamData on_stream_data,
                OnSessionClose on_session_close = nullptr);

    bool Start(const char *cert_file, const char *key_file,
               const char *ip, int port);
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

    WTServerSession *EnsureSession(wt_session_t *sess, const std::string &path);
    void RemoveSession(wt_session_t *sess);

    uv_loop_t *loop_ = nullptr;
    wt_server_t *server_ = nullptr;
    bool listening_ = false;
    std::vector<std::unique_ptr<PathCbCtx>> path_ctxs_;
    std::unordered_map<wt_session_t *, std::unique_ptr<WTServerSession>> sessions_;
};

} /* namespace cpp_streamer */

#endif /* WT_SERVER_HPP */
