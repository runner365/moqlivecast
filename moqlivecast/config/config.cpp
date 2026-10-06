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

/* ALPN 候选表。缺省保持历史行为：h3（浏览器 WebTransport）+ quic-echo（测试）。
 * 加 "moq-lite-06" 就同时在这个 UDP 端口上开放裸 QUIC 的 moq-lite。 */
static bool LoadAlpnList(const YAML::Node &node,
                         std::vector<std::string> &out) {
    if (!node || !node.IsSequence() || node.size() == 0) {
        LOG_ERROR("[config] alpn must be a non-empty list");
        return false;
    }
    out.clear();
    for (const auto &item : node) {
        std::string name = TrimCopy(item.as<std::string>());
        if (name.empty()) {
            LOG_ERROR("[config] alpn item is empty");
            return false;
        }
        if (name.size() > 255) {
            LOG_ERROR("[config] alpn item too long: %s", name.c_str());
            return false;
        }
        /* 重复项没有意义（选择器取第一个命中），还说明配置写错了 —— 直接报错，
         * 别让它在握手时变成一个看不懂的现象 */
        for (const auto &seen : out) {
            if (seen == name) {
                LOG_ERROR("[config] duplicate alpn entry: %s", name.c_str());
                return false;
            }
        }
        out.push_back(std::move(name));
    }
    return true;
}

/* key 只用来写日志：subpath 与 moq_lite_subpath 的校验规则一样，报错时要能
 * 看出是哪一项写坏了。 */
static bool LoadSubPathList(const char *key, const YAML::Node &node,
                            std::vector<SubPathItem> &out) {
    if (!node || !node.IsSequence() || node.size() == 0) {
        LOG_ERROR("[config] %s must be a non-empty list", key);
        return false;
    }
    out.clear();
    for (const auto &item : node) {
        if (!item || !item.IsMap() || !item["path"]) {
            LOG_ERROR("[config] %s item needs path", key);
            return false;
        }
        SubPathItem sp;
        sp.path = NormalizePath(item["path"].as<std::string>());
        if (item["desc"]) {
            sp.desc = item["desc"].as<std::string>();
        }
        if (sp.path.empty() || sp.path == "/") {
            LOG_ERROR("[config] invalid %s path", key);
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
            if (!LoadSubPathList("subpath", srv["subpath"], sub_paths_)) {
                return false;
            }
        }
        /* moq-lite 的路由表。缺省 "anon" 的理由：
         *  1) moq-cli / libmoq 生态的匿名入口约定就是 /anon
         *     （目标命令 `moql://<host>:4433/anon` 零改动可跑）；
         *  2) 这是**新增**的一项，缺省值不会改变任何既有部署的行为 ——
         *     只有 alpn 表里写了 moq-lite-* 才会走到这里。
         * 想彻底关掉 moq-lite 就把 alpn 里的 moq-lite-06 去掉；写了这一项
         * 但给空列表是配置错误（同 subpath），不给"空表"这个中间态。 */
        if (srv["moq_lite_subpath"]) {
            if (!LoadSubPathList("moq_lite_subpath", srv["moq_lite_subpath"],
                                 moq_lite_sub_paths_)) {
                return false;
            }
        } else {
            SubPathItem sp;
            sp.desc = "moq-lite anonymous entry";
            sp.path = "/anon";
            moq_lite_sub_paths_.push_back(std::move(sp));
        }
        /* alpn 缺省 = 历史默认值，保证不写这项时行为完全不变 */
        if (srv["alpn"]) {
            if (!LoadAlpnList(srv["alpn"], alpn_list_)) {
                return false;
            }
        } else {
            alpn_list_ = { "h3", "quic-echo" };
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
