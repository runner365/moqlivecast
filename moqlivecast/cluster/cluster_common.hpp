#ifndef CLUSTER_COMMON_HPP
#define CLUSTER_COMMON_HPP

#include <cstdint>
#include <string>

namespace cpp_streamer {

/* Edge 出站链路的参数快照（从 Config 拷一份，运行期不再碰单例）。 */
struct ClusterOptions {
    std::string center_host;
    int center_port = 4433;
    std::string center_path = "anon";
    int reconnect_min_ms = 500;
    int reconnect_max_ms = 10000;
    int pull_acquire_timeout_ms = 5000;
    int pull_idle_timeout_ms = 30000;
    int negative_cache_ms = 5000;
    int max_pull_connections = 128;
};

/* moql://host:port/path */
std::string CenterUrl(const ClusterOptions &opts);

/* 本地 key（"app/stream..."）→ 去掉第一段路径后的名字。
 * 例：anon/123456 → 123456，anon/live1/stream1 → live1/stream1。
 *
 * ⚠️ 集群有两个不同的「wire」概念，别混：
 *   - **上推/回源时的广播名** = 源端完整 key（app/stream），由
 *     ClusterManager::StartPush / EnsurePulled 的 wire 参数显式传递；
 *     中心侧 key 因此是 <center_path>/<app>/<stream>，同名不同 app 不冲突。
 *   - **对端公告名** = 本函数：Center 把 <center_path>/<app>/<stream>
 *     还原成 <app>/<stream> 公告给 Edge；Edge 的本地公告仍只报 stream 名。 */
bool WireBroadcastOf(const std::string &local_key, std::string *wire);

/* SplitLocalKey("anon/123456", &app, &stream) */
bool SplitLocalKey(const std::string &local_key, std::string *app,
                   std::string *stream);

int64_t ClusterNowMs();

/* 指数退避 + 抖动：[min, max]，按 attempt 倍增。
 * 抖动是确定性的（按 key 与 now 混合），避免中心重启时 N 条连接同拍重连。 */
int ClusterBackoffMs(const ClusterOptions &opts, int attempt,
                     const std::string &key, int64_t now_ms);

} /* namespace cpp_streamer */

#endif /* CLUSTER_COMMON_HPP */
