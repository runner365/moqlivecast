#include "cluster_manager.hpp"

#include "logger.h"

#include <cstdio>
#include <cstdlib>

namespace cpp_streamer {

namespace {
/* 没有播放器、也没有新需求，多久之后关回源连接。 */
constexpr int64_t kPullIdleDemandMs = 5000;
constexpr int kTickMs = 100;

/* 进程收尾时故意不析构的连接对象：关连接是异步的，on_close 可能在管理器
 * 析构之后才回来。进程马上退出，泄漏一份状态比 use-after-free 好。 */
std::vector<void *> g_retired_links;

int ParseHostPort(const std::string &addr, std::string *host, int *port) {
    const size_t colon = addr.find(':');
    if (colon == std::string::npos) {
        *host = addr;
        *port = 4433;
        return 0;
    }
    *host = addr.substr(0, colon);
    const std::string p = addr.substr(colon + 1);
    char *end = nullptr;
    const long v = std::strtol(p.c_str(), &end, 10);
    if (p.empty() || !end || *end != '\0' || v <= 0 || v > 65535) return -1;
    *port = static_cast<int>(v);
    return 0;
}
} /* namespace */

ClusterManager *ClusterManager::s_instance_ = nullptr;

ClusterManager::ClusterManager(uv_loop_t *loop)
    : TimerInterface(kTickMs), loop_(loop) {}

ClusterManager::~ClusterManager() {
    Shutdown();
}

bool ClusterManager::Init(const Config &cfg) {
    enabled_ = (cfg.ClusterModeValue() == ClusterMode::kEdge);

    if (!enabled_) {
        LOG_INFO("[cluster] disabled (mode=off; a center is a plain server)");
        return true;
    }

    opts_.center_path = cfg.ClusterCenterPath().empty() ? "anon"
                                                        : cfg.ClusterCenterPath();
    opts_.reconnect_min_ms = cfg.ClusterReconnectMinMs();
    opts_.reconnect_max_ms = cfg.ClusterReconnectMaxMs();
    opts_.pull_acquire_timeout_ms = cfg.ClusterPullAcquireTimeoutMs();
    opts_.pull_idle_timeout_ms = cfg.ClusterPullIdleTimeoutMs();
    opts_.negative_cache_ms = cfg.ClusterNegativeCacheMs();
    opts_.max_pull_connections = cfg.ClusterMaxPullConnections();

    /* edge */
    if (cfg.ClusterProtocol() != "moq") {
        LOG_ERROR("[cluster] protocol \"%s\" is not implemented in phase 1",
                  cfg.ClusterProtocol().c_str());
        return false;
    }
    if (ParseHostPort(cfg.ClusterCenterAddress(), &opts_.center_host,
                      &opts_.center_port) != 0) {
        LOG_ERROR("[cluster] invalid center_address \"%s\"",
                  cfg.ClusterCenterAddress().c_str());
        return false;
    }
    enabled_ = true;
    LOG_INFO("[cluster] mode=edge, center=%s:%d path=\"%s\", max_pulls=%d",
             opts_.center_host.c_str(), opts_.center_port,
             opts_.center_path.c_str(), cfg.ClusterMaxPullConnections());
    return true;
}

void ClusterManager::Start() {
    if (!enabled_) return;
    s_instance_ = this;
    MediaStreamManager::AddStreamCallback(this);
    StartTimer();

    /* 进程启动时已经在推的流（很少见，但重启集群进程时会遇到）。 */
    const std::vector<std::string> keys = MediaStreamManager::ListPublisherKeys();
    for (const std::string &key : keys) {
        std::string json;
        uint64_t rev = 0;
        if (!MediaStreamManager::GetCatalogJson(key, json, &rev)) continue;
        StartPush(key, json);
    }

    /* 目录链路：让本地客户端看得见 Center 上的广播，SUBSCRIBE 才有触发点。 */
    discovery_.reset(new ClusterDiscovery(
        loop_, opts_, [this](const std::string &wire, bool added) {
            OnRemoteWire(wire, added);
        }));
    discovery_->Start();
}

void ClusterManager::SetAnnounceSink(AnnounceSink sink) {
    announce_sink_ = std::move(sink);
}

bool ClusterManager::IsLocalWire(const std::string &wire) const {
    const std::vector<std::string> keys = MediaStreamManager::ListPublisherKeys();
    std::string local_wire, json;
    for (const std::string &key : keys) {
        /* 新命名空间：远端 wire 就是源端的完整 app/stream，和本地 key 直接比。
         * 旧命名空间（wire 只有 stream 名）保留第二种比较，兼容老客户端。 */
        bool match = (key == wire);
        if (!match && WireBroadcastOf(key, &local_wire)) {
            match = (local_wire == wire);
        }
        if (!match) continue;
        if (MediaStreamManager::GetCatalogJson(key, json)) return true;
    }
    return false;
}

void ClusterManager::OnRemoteWire(const std::string &wire, bool added) {
    if (!enabled_ || wire.empty()) return;
    if (added) {
        if (!remote_wires_.insert(wire).second) return;
    } else {
        if (remote_wires_.erase(wire) == 0) return;
    }
    /* 本地推流同名时，公告状态由本地 catalog 那条路维护，别被远端覆盖。 */
    if (IsLocalWire(wire)) return;
    if (announce_sink_) announce_sink_(wire, added);
}

void ClusterManager::AppendRemoteBroadcasts(
    std::vector<std::string> *out) const {
    if (!out) return;
    for (const std::string &wire : remote_wires_) {
        if (IsLocalWire(wire)) continue;
        out->push_back(wire);
    }
}

void ClusterManager::Shutdown() {
    StopTimer();
    if (s_instance_ == this) s_instance_ = nullptr;

    /* ⚠️ 必须从 MediaStreamManager 的回调表里摘掉自己。否则本对象析构之后，
     * 最后一个 publisher 的 RemovePublisher 仍会遍历 cb_vec_ 调到这里（虚表
     * 已经失效）→ 实测带活跃推流 Ctrl+C 时 SIGSEGV 在 RemovePublisher 里。 */
    MediaStreamManager::RemoveStreamCallback(this);

    if (discovery_) {
        discovery_->Stop();
        g_retired_links.push_back(discovery_.release());
    }
    remote_wires_.clear();

    for (auto &item : pushes_) {
        if (item.second) {
            item.second->Stop();
            g_retired_links.push_back(item.second.release());
        }
    }
    pushes_.clear();

    for (auto &item : pulls_) {
        if (item.second) {
            item.second->Stop();
            g_retired_links.push_back(item.second.release());
        }
    }
    pulls_.clear();
    negative_cache_.clear();
    enabled_ = false;
}

bool ClusterManager::IsPulled(const std::string &local_key) const {
    return pulls_.find(local_key) != pulls_.end();
}

void ClusterManager::StartPush(const std::string &local_key,
                               const std::string &catalog_json) {
    if (!enabled_) return;
    if (catalog_json.empty()) return;
    if (IsPulled(local_key)) return;   /* 防环：回源来的流不再上推 */

    /* 集群 wire = 源端的完整 app/stream，而不是只取 stream 名。
     * 否则 live1/stream1 和 live2/stream1 在 Center 都会变成 anon/stream1，
     * 第二条上推会 takeover 第一条、公告也只剩一个名字。
     * Center 侧 key 因此是 <center_path>/<app>/<stream>（如
     * anon/live1/stream1），同名不同 app 互不冲突。 */
    std::string app, stream;
    if (!SplitLocalKey(local_key, &app, &stream)) {
        LOG_DEBUG("[cluster] key \"%s\" has no app/stream split, not pushing",
                  local_key.c_str());
        return;
    }
    const std::string wire = local_key;

    auto it = pushes_.find(local_key);
    if (it != pushes_.end() && it->second) {
        it->second->Resume(catalog_json);
        return;
    }

    auto link = std::unique_ptr<ClusterPushLink>(
        new ClusterPushLink(loop_, opts_, local_key, wire, catalog_json));
    ClusterPushLink *raw = link.get();
    pushes_[local_key] = std::move(link);
    raw->Start();
}

void ClusterManager::OnCatalogUpdated(const std::string &stream_key,
                                      uint64_t rev) {
    (void)rev;
    if (!enabled_) return;
    if (IsPulled(stream_key)) return;

    std::string json;
    if (!MediaStreamManager::GetCatalogJson(stream_key, json)) return;
    StartPush(stream_key, json);
}

void ClusterManager::OnPublish(const std::string &app,
                               const std::string &streamname) {
    (void)app;
    (void)streamname;
    /* catalog 到达时才会建上推连接（没有 catalog 就发不了 moq-lite）。 */
}

void ClusterManager::OnUnpublish(const std::string &app,
                                 const std::string &streamname) {
    if (!enabled_) return;
    const std::string key = app + "/" + streamname;

    /* 回源停止时自己会 ReleasePublisher，也会走到这里；不要把它当成
     * 本地上推的退订。 */
    if (IsPulled(key)) return;

    auto it = pushes_.find(key);
    if (it != pushes_.end() && it->second) {
        it->second->Pause();
    }
}

bool ClusterManager::EnsurePulled(const std::string &local_key,
                                  const std::string &wire) {
    if (!enabled_ || wire.empty()) return false;

    const int64_t now = ClusterNowMs();

    auto pit = pulls_.find(local_key);
    if (pit != pulls_.end() && pit->second) {
        pit->second->TouchDemand(now);
        if (pit->second->Stopped()) pit->second->Start();
        return true;
    }

    /* 本地已经有推流：等它的 catalog，不需要回源。 */
    if (pushes_.find(local_key) != pushes_.end()) return false;
    if (MediaStreamManager::HasPublisher(local_key)) return true;

    auto nit = negative_cache_.find(local_key);
    if (nit != negative_cache_.end()) {
        if (now < nit->second) return false;
        negative_cache_.erase(nit);
    }

    if (pulls_.size() >= static_cast<size_t>(opts_.max_pull_connections)) {
        LOG_WARN("[cluster] pull %s rejected: %zu concurrent pulls (max)",
                 local_key.c_str(), pulls_.size());
        return false;
    }

    char owner[192];
    snprintf(owner, sizeof(owner), "cluster-pull:%s#%llu", local_key.c_str(),
             static_cast<unsigned long long>(++pull_seq_));
    auto link = std::unique_ptr<ClusterPullLink>(
        new ClusterPullLink(loop_, opts_, local_key, wire, owner));
    ClusterPullLink *raw = link.get();
    pulls_[local_key] = std::move(link);
    raw->Start();
    LOG_INFO("[cluster] pull %s requested (owner=%s)", local_key.c_str(), owner);
    return true;
}

void ClusterManager::PlayerRequestTrampoline(const std::string &local_key) {
    ClusterManager *self = s_instance_;
    if (!self || !self->enabled_) return;
    /* /moq / RTMP / HTTP-FLV 播放请求里的 local_key 就是 app/stream，
     * 直接把它作为回源的源端身份。 */
    self->EnsurePulled(local_key, local_key);
}

bool ClusterManager::OnTimer() {
    const int64_t now = ClusterNowMs();

    if (discovery_) discovery_->OnTick(now);

    for (auto it = pushes_.begin(); it != pushes_.end();) {
        it->second->OnTick(now);
        if (it->second->FullyStopped()) {
            it = pushes_.erase(it);
        } else {
            ++it;
        }
    }

    for (auto it = pulls_.begin(); it != pulls_.end();) {
        ClusterPullLink *link = it->second.get();
        link->OnTick(now);

        if (link->FullyStopped()) {
            if (link->AcquireTimedOut() && opts_.negative_cache_ms > 0) {
                negative_cache_[it->first] = now + opts_.negative_cache_ms;
            }
            it = pulls_.erase(it);
            continue;
        }

        /* 没有播放器、也没有新需求：关连接（本地 publisher 马上会被释放）。 */
        if (!link->Stopped() &&
            MediaStreamManager::PlayerCount(it->first) == 0 &&
            now - link->LastDemandMs() > kPullIdleDemandMs) {
            link->Stop();
        }
        ++it;
    }

    return true;
}

} /* namespace cpp_streamer */
