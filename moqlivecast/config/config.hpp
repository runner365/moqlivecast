#ifndef MEDIA_OVER_WT_CONFIG_HPP
#define MEDIA_OVER_WT_CONFIG_HPP

#include "logger.h"

#include <string>
#include <vector>

namespace cpp_streamer {

struct SubPathItem {
    std::string desc;
    std::string path;
};

/* 两级集群里的本机角色。kOff = 不参与集群（缺省）。
 *
 * 没有 center：中心节点就是一个**普通服务端** —— 它不需要知道自己被谁拨，
 * Edge 用 `center_address` 主动连上来即可。给它一个只改日志的 mode 只会
 * 制造「写了 center 却没生效」的误解。 */
enum class ClusterMode {
    kOff,
    kEdge,
};

class Config {
public:
    static Config &Instance();

    bool Load(const std::string &filename);

    const std::string &ListenIp() const { return listen_ip_; }
    int Port() const { return port_; }
    const std::vector<SubPathItem> &SubPaths() const { return sub_paths_; }
    /* 裸 QUIC 的 moq-lite 路由表，与上面那张**故意分开**：
     *   subpath 的 path 是 WebTransport 的 URL 路径（配合 h3）；
     *   这里的 path 是 moq-lite SETUP 的 Path 参数，也就是发布端的 origin
     *   （moql://host:4433/anon 里的 anon）。
     * 合成一张表会让两条线的路径互相污染，而浏览器那条线本轮不动。
     * 缺省 = {"anon"}，见 config.cpp 里的理由。 */
    const std::vector<SubPathItem> &MoqLiteSubPaths() const {
        return moq_lite_sub_paths_;
    }
    const std::string &CertFile() const { return cert_file_; }
    const std::string &KeyFile() const { return key_file_; }
    /* 本端口接受的应用协议（ALPN），服务端偏好优先。
     * 缺省 = {"h3", "quic-echo"}，与加这个配置项之前的行为一致。
     * 裸 QUIC 的 moq-lite 就是靠在这张表里加 "moq-lite-06" 与 WebTransport
     * 共用同一个 UDP 端口 —— 包按连接 ID 分，协议在握手时定一次。 */
    const std::vector<std::string> &AlpnList() const { return alpn_list_; }

    const std::string &HttpFlvListenIp() const { return httpflv_listen_ip_; }
    int HttpFlvPort() const { return httpflv_port_; }

    /* ── RTMP server ──
     * 与 QUIC/HTTP-FLV 分开的 TCP 端口（惯例 1935）。缺省**关闭**：不写
     * `rtmp` 这一节、或写了但不给 `enable: true`，都不会监听 —— 新增端口
     * 不该改变既有部署的行为。字段即使关闭也会解析，方便先写好配置。
     *
     * idle_timeout_ms 是「握手完成后这么久没有任何读写就关闭会话」的阈值；
     * 原来的实现写死 15 秒，这里把它变成可配置项（范围 1s ~ 10min）。 */
    bool RtmpEnabled() const { return rtmp_enabled_; }
    const std::string &RtmpListenIp() const { return rtmp_listen_ip_; }
    int RtmpPort() const { return rtmp_port_; }
    int RtmpIdleTimeoutMs() const { return rtmp_idle_timeout_ms_; }

    /* ── Cluster（Edge / Center）──
     * mode=center 只是角色标记（服务端行为不变）；mode=edge 时会向
     * center_address 建出站 moq-lite 连接（每路一条），并在本地缺流时回源。 */
    ClusterMode ClusterModeValue() const { return cluster_mode_; }
    const std::string &ClusterProtocol() const { return cluster_protocol_; }
    const std::string &ClusterCenterAddress() const { return cluster_center_address_; }
    const std::string &ClusterCenterPath() const { return cluster_center_path_; }
    int ClusterReconnectMinMs() const { return cluster_reconnect_min_ms_; }
    int ClusterReconnectMaxMs() const { return cluster_reconnect_max_ms_; }
    int ClusterSubscribeTimeoutMs() const { return cluster_subscribe_timeout_ms_; }
    int ClusterPullAcquireTimeoutMs() const { return cluster_pull_acquire_timeout_ms_; }
    int ClusterPullIdleTimeoutMs() const { return cluster_pull_idle_timeout_ms_; }
    int ClusterMaxPullConnections() const { return cluster_max_pull_connections_; }
    int ClusterNegativeCacheMs() const { return cluster_negative_cache_ms_; }

    LogLevel LogLevelValue() const { return log_level_; }
    const std::string &LogFilename() const { return log_filename_; }
    bool LogConsole() const { return log_console_; }

    const std::string &MetaLogFile() const { return meta_log_file_; }
    int MetaLogInterval() const { return meta_log_interval_; }

    Config(const Config &) = delete;
    Config &operator=(const Config &) = delete;

private:
    Config() = default;
    ~Config() = default;

    static LogLevel ParseLogLevel(const std::string &s);

    std::string listen_ip_ = "0.0.0.0";
    int port_ = 4433;
    std::vector<SubPathItem> sub_paths_;
    std::vector<SubPathItem> moq_lite_sub_paths_;
    std::string cert_file_;
    std::string key_file_;
    std::vector<std::string> alpn_list_;

    std::string httpflv_listen_ip_ = "0.0.0.0";
    int httpflv_port_ = 4433;

    /* RTMP：TCP，独立端口。缺省关闭（见 RtmpEnabled 的说明）。 */
    bool rtmp_enabled_ = false;
    std::string rtmp_listen_ip_ = "0.0.0.0";
    int rtmp_port_ = 1935;
    int rtmp_idle_timeout_ms_ = 15000;

    /* Cluster：缺省关闭，不改变既有部署。 */
    ClusterMode cluster_mode_ = ClusterMode::kOff;
    std::string cluster_protocol_ = "moq";
    std::string cluster_center_address_;
    std::string cluster_center_path_ = "anon";
    int cluster_reconnect_min_ms_ = 500;
    int cluster_reconnect_max_ms_ = 10000;
    int cluster_subscribe_timeout_ms_ = 3000;
    int cluster_pull_acquire_timeout_ms_ = 5000;
    int cluster_pull_idle_timeout_ms_ = 30000;
    int cluster_max_pull_connections_ = 128;
    int cluster_negative_cache_ms_ = 5000;

    LogLevel log_level_ = INFO;
    std::string log_filename_ = "/tmp/moqlivecast.log";
    bool log_console_ = false;

    std::string meta_log_file_ = "/tmp/moq_meta.log";
    int meta_log_interval_ = 3;
};

} /* namespace cpp_streamer */

#endif /* MEDIA_OVER_WT_CONFIG_HPP */
