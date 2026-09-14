#include "config.hpp"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <yaml-cpp/yaml.h>

namespace cpp_streamer {

Config &Config::Instance() {
    static Config inst;
    return inst;
}

static std::string NormalizePath(const std::string &path) {
    if (path.empty()) return {};
    if (path[0] == '/') return path;
    return "/" + path;
}

static std::string TrimCopy(const std::string &s) {
    size_t b = 0;
    size_t e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) {
        ++b;
    }
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) {
        --e;
    }
    return s.substr(b, e - b);
}

static std::string YamlStringOrEmpty(const YAML::Node &node) {
    if (!node || node.IsNull()) {
        return {};
    }
    try {
        return TrimCopy(node.as<std::string>());
    } catch (const YAML::Exception &) {
        return {};
    }
}

static std::string EnvOrEmpty(const char *name) {
    const char *v = std::getenv(name);
    if (!v || !v[0]) {
        return {};
    }
    return TrimCopy(v);
}

static void ConfigFail(const char *msg) {
    std::fprintf(stderr, "%s\n", msg);
    LOG_ERROR("%s", msg);
}

static bool LoadSubPathList(const YAML::Node &node,
                            std::vector<SubPathItem> &out) {
    if (!node || !node.IsSequence() || node.size() == 0) {
        LOG_ERROR("[config] subpath must be a non-empty list");
        return false;
    }
    out.clear();
    for (const auto &item : node) {
        if (!item || !item.IsMap() || !item["path"]) {
            LOG_ERROR("[config] subpath item needs path");
            return false;
        }
        SubPathItem sp;
        sp.path = NormalizePath(item["path"].as<std::string>());
        if (item["desc"]) {
            sp.desc = item["desc"].as<std::string>();
        }
        if (sp.path.empty() || sp.path == "/") {
            LOG_ERROR("[config] invalid subpath path");
            return false;
        }
        out.push_back(std::move(sp));
    }
    return true;
}

LogLevel Config::ParseLogLevel(const std::string &s) {
    std::string v = s;
    for (char &c : v) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    if (v == "debug") return DEBUG;
    if (v == "warn" || v == "warning") return WARN;
    if (v == "error") return ERROR;
    return INFO;
}

bool Config::Load(const std::string &filename) {
    try {
        YAML::Node root = YAML::LoadFile(filename);
        if (!root || root.IsNull()) {
            LOG_ERROR("[config] empty file: %s", filename.c_str());
            return false;
        }

        YAML::Node srv = root["moq_server"];
        if (!srv || !srv.IsMap()) {
            LOG_ERROR("[config] missing moq_server section");
            return false;
        }
        if (srv["listenip"]) {
            listen_ip_ = srv["listenip"].as<std::string>();
        }
        if (srv["port"]) {
            port_ = srv["port"].as<int>();
        }
        if (srv["subpath"]) {
            if (!LoadSubPathList(srv["subpath"], sub_paths_)) {
                return false;
            }
        }
        cert_file_ = YamlStringOrEmpty(srv["certfile"]);
        key_file_ = YamlStringOrEmpty(srv["keyfile"]);

        YAML::Node httpflv = root["httpflv"];
        if (httpflv && httpflv.IsMap()) {
            if (httpflv["listenip"]) {
                httpflv_listen_ip_ = httpflv["listenip"].as<std::string>();
            }
            if (httpflv["port"]) {
                httpflv_port_ = httpflv["port"].as<int>();
            }
        }

        YAML::Node log = root["log"];
        if (log && log.IsMap()) {
            if (log["level"]) {
                log_level_ = ParseLogLevel(log["level"].as<std::string>());
            }
            if (log["filename"]) {
                log_filename_ = log["filename"].as<std::string>();
            }
            if (log["console"]) {
                log_console_ = log["console"].as<bool>();
            }
        }

        YAML::Node meta = root["meta_log"];
        if (meta && meta.IsMap()) {
            if (meta["log"]) {
                meta_log_file_ = YamlStringOrEmpty(meta["log"]);
            }
            if (meta["interval"]) {
                meta_log_interval_ = meta["interval"].as<int>();
            }
        }
        if (meta_log_interval_ < 2) meta_log_interval_ = 2;
        if (meta_log_interval_ > 10) meta_log_interval_ = 10;
    } catch (const YAML::Exception &e) {
        LOG_ERROR("[config] parse %s failed: %s", filename.c_str(), e.what());
        return false;
    }

    if (listen_ip_.empty() || port_ <= 0 || port_ > 65535) {
        LOG_ERROR("[config] invalid listen %s:%d", listen_ip_.c_str(), port_);
        return false;
    }
    if (sub_paths_.empty()) {
        LOG_ERROR("[config] subpath required");
        return false;
    }
    if (cert_file_.empty() && key_file_.empty()) {
        cert_file_ = EnvOrEmpty("SSL_CERT_FILE");
        key_file_ = EnvOrEmpty("SSL_KEY_FILE");
    }
    if (cert_file_.empty() || key_file_.empty()) {
        ConfigFail("[config] certfile/keyfile are empty; set them in YAML "
                   "or export SSL_CERT_FILE and SSL_KEY_FILE");
        return false;
    }
    if (httpflv_listen_ip_.empty() || httpflv_port_ <= 0 || httpflv_port_ > 65535) {
        LOG_ERROR("[config] invalid httpflv listen %s:%d",
                  httpflv_listen_ip_.c_str(), httpflv_port_);
        return false;
    }
    return true;
}

} /* namespace cpp_streamer */
