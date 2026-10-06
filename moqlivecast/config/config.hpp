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

    LogLevel log_level_ = INFO;
    std::string log_filename_ = "/tmp/moqlivecast.log";
    bool log_console_ = false;

    std::string meta_log_file_ = "/tmp/moq_meta.log";
    int meta_log_interval_ = 3;
};

} /* namespace cpp_streamer */

#endif /* MEDIA_OVER_WT_CONFIG_HPP */
