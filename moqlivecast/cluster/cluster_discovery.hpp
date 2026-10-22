#ifndef CLUSTER_DISCOVERY_HPP
#define CLUSTER_DISCOVERY_HPP

#include "cluster_common.hpp"

#include "moq_lite_client.h"

#include <functional>
#include <map>
#include <set>
#include <string>

namespace cpp_streamer {

/* Edge 的发现链路：一条只读的 SUBSCRIBER 连接，`ANNOUNCE_REQUEST` 拿到 Center
 * 当前有哪些广播，并在 Center 补发 ANNOUNCE_START / ANNOUNCE_END 时更新。
 *
 * 目的：让本地 moq-lite 客户端**看得见**远端广播，否则它们不会发 SUBSCRIBE，
 * 按需回源永远没有触发点。这条连接不订媒体，只做目录。 */
class ClusterDiscovery {
public:
    /* wire = 线上广播名（不含入口路径）；added=false 表示撤下。 */
    using Sink = std::function<void(const std::string &wire, bool added)>;

    ClusterDiscovery(uv_loop_t *loop, const ClusterOptions &opts, Sink sink);
    ~ClusterDiscovery();

    ClusterDiscovery(const ClusterDiscovery &) = delete;
    ClusterDiscovery &operator=(const ClusterDiscovery &) = delete;

    void Start();
    void Stop();
    void OnTick(int64_t now_ms);
    bool FullyStopped() const { return stopped_ && session_ == 0; }

private:
    static void OnConnected(moq_lite_session_t s, int status, void *user);
    static void OnClosed(moq_lite_session_t s, int err, const char *reason,
                         void *user);
    static void OnAnnounce(moq_lite_session_t s, moq_lite_broadcast_t bc,
                           const char *path, const char *suffix, void *user);
    static void OnAnnounceEnd(moq_lite_session_t s, moq_lite_broadcast_t bc,
                              void *user);

    void CreateSession();
    void HandleClosed(int err);
    void ClearKnown(bool notify);
    void FreeSessionIfClosed(int64_t now_ms);

private:
    uv_loop_t *loop_ = nullptr;
    ClusterOptions opts_;
    Sink sink_;

    moq_lite_session_t session_ = 0;
    std::set<std::string> known_;
    std::map<moq_lite_broadcast_t, std::string> bc_paths_;

    bool connecting_ = false;
    bool stopped_ = false;
    bool close_pending_ = false;
    bool session_dead_ = false;
    int64_t free_at_ms_ = 0;
    int64_t retry_at_ms_ = 0;
    int attempt_ = 0;
};

} /* namespace cpp_streamer */

#endif /* CLUSTER_DISCOVERY_HPP */
