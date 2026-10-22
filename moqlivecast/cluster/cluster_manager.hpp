#ifndef CLUSTER_MANAGER_HPP
#define CLUSTER_MANAGER_HPP

#include "cluster_common.hpp"
#include "cluster_discovery.hpp"
#include "cluster_pull.hpp"
#include "cluster_push.hpp"
#include "config/config.hpp"
#include "media_stream_manager.hpp"
#include "timer.hpp"

#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace cpp_streamer {

/* 两级集群的 Edge 侧控制器。
 *
 * - 本地流出现 catalog → 建一条上推连接（每条流一条）；
 * - 本地缺流被请求 → 建一条回源连接（每条流一条）；
 * - 所有连接都在同一条 uv loop 上，回调里只做状态标记，删除延后到 tick。
 *
 * mode=center 时 Enabled() 为 false：中心端行为与单机服务端一致。
 */
class ClusterManager : public StreamManagerCallbackI, public TimerInterface {
public:
    explicit ClusterManager(uv_loop_t *loop);
    ~ClusterManager();

    bool Init(const Config &cfg);
    void Start();
    void Shutdown();

    bool Enabled() const { return enabled_; }

    void OnCatalogUpdated(const std::string &stream_key, uint64_t rev);

    /* 本地缺流的统一入口。返回 true = 已有一条回源在跑（或本地正在推、
     * catalog 稍后到），调用方可以等待；false = 不会有了，应直接拒绝。
     *
     * wire 是发到 Center 的广播名，**必须显式给出源端的 app/stream 身份**：
     *   - /moq（WebTransport）/ RTMP / HTTP-FLV 播放：local_key 本身就是
     *     播放请求里的 app/stream，直接传 local_key；
     *   - moq-lite 订阅：传对端 SUBSCRIBE 里的 broadcast（远端广播已经是
     *     app-qualified 的名字）。
     * 不能从 local_key 反推 —— 回源侧的 local_key 里那一段 app 是本端路由
     * 路径，不是源的 app（见 ClusterManager::EnsurePulled 的说明）。 */
    bool EnsurePulled(const std::string &local_key, const std::string &wire);

    /* MediaStreamManager::PLAY_CALLBACK 的 trampoline（函数指针，不能绑
     * lambda）。RTMP play / HTTP-FLV 请求进来时触发。 */
    static void PlayerRequestTrampoline(const std::string &local_key);

    /* Center 公告集合的变化 → 本地订阅方的 announce 流（由 moq.cpp 接到
     * MoqLiteServer::NotifyRemoteAnnounce）。 */
    using AnnounceSink = std::function<void(const std::string &wire, bool added)>;
    void SetAnnounceSink(AnnounceSink sink);
    /* 回答本地 ANNOUNCE_REQUEST 时合并进快照。 */
    void AppendRemoteBroadcasts(std::vector<std::string> *out) const;

    /* StreamManagerCallbackI */
    void OnPublish(const std::string &app, const std::string &streamname) override;
    void OnUnpublish(const std::string &app, const std::string &streamname) override;

    /* TimerInterface */
    bool OnTimer() override;

private:
    void StartPush(const std::string &local_key, const std::string &catalog_json);
    bool IsPulled(const std::string &local_key) const;
    void OnRemoteWire(const std::string &wire, bool added);
    bool IsLocalWire(const std::string &wire) const;

private:
    uv_loop_t *loop_ = nullptr;
    bool enabled_ = false;   /* true = edge（唯一的出站角色） */
    ClusterOptions opts_;

    std::map<std::string, std::unique_ptr<ClusterPushLink>> pushes_;
    std::map<std::string, std::unique_ptr<ClusterPullLink>> pulls_;
    std::map<std::string, int64_t> negative_cache_;   /* key -> expire ms */
    uint64_t pull_seq_ = 0;

    std::unique_ptr<ClusterDiscovery> discovery_;
    std::set<std::string> remote_wires_;
    AnnounceSink announce_sink_;

    static ClusterManager *s_instance_;
};

} /* namespace cpp_streamer */

#endif /* CLUSTER_MANAGER_HPP */
