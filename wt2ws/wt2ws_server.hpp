#ifndef WT2WS_SERVER_HPP
#define WT2WS_SERVER_HPP

#include "wt2ws_config.hpp"
#include "wt2ws_session.hpp"

#include "webtransport_server_api.h"

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace cpp_streamer {
namespace wt2ws {

class Wt2wsServer {
public:
    explicit Wt2wsServer(uv_loop_t *loop);
    ~Wt2wsServer();

    Wt2wsServer(const Wt2wsServer &) = delete;
    Wt2wsServer &operator=(const Wt2wsServer &) = delete;

    bool Start(const Wt2wsConfig &cfg, std::string *err);
    void Stop();

    /* WebTransport path callbacks (called on the loop thread). */
    void OnSession(wt_session_t *sess);
    void OnStreamData(wt_session_t *sess, wt_stream_t *st,
                      const uint8_t *data, size_t len);
    void OnSessionClose(wt_session_t *sess);
    void OnTick();

    const Wt2wsConfig &config() const { return cfg_; }

private:
    static void OnSessionTramp(wt_server_t *srv, wt_session_t *sess, void *user);
    static void OnStreamTramp(wt_server_t *srv, wt_session_t *sess,
                              wt_stream_t *st, const uint8_t *data, size_t len,
                              void *user);
    static void OnCloseTramp(wt_server_t *srv, wt_session_t *sess, void *user);
    static void OnIdleTimer(uv_timer_t *timer);

    uv_loop_t *loop_ = nullptr;
    wt_server_t *srv_ = nullptr;
    Wt2wsConfig cfg_;
    std::map<uint64_t, std::shared_ptr<Wt2wsSession>> sessions_;
    /* Closed sessions stay alive until process exit: libuv / QUIC close
     * callbacks can still reference them after the WT session is freed.
     * Same rationale as cluster's retired links. */
    std::vector<std::shared_ptr<Wt2wsSession>> retired_;
    uv_timer_t idle_timer_;
    bool idle_timer_started_ = false;
};

} /* namespace wt2ws */
} /* namespace cpp_streamer */

#endif /* WT2WS_SERVER_HPP */
