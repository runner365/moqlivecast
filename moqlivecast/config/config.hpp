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
    const std::string &CertFile() const { return cert_file_; }
    const std::string &KeyFile() const { return key_file_; }

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
    std::string cert_file_;
    std::string key_file_;

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
