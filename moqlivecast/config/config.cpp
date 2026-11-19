#include "config.hpp"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <set>
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

/* media_sources 是静态本地源配置：
 *   media_sources:
 *     - id: program-1
 *       type: flv
 *       app: live
 *       stream: program1
 *       paths: [/data/program1.flv, /data/program2.flv]
 *       realtime: true
 *       loop: true
 *
 * paths 是节目单，按文件顺序连续播放；loop=true 时最后一个文件播完回到
 * 第一个文件。每个文件切换时由 source 重新发 sequence header，并将 DTS/PTS
 * 接到上一条时间轴后面。 */
static bool LoadMediaSourceList(const YAML::Node &node,
                                std::vector<MediaSourceConfig> &out) {
    if (!node.IsSequence()) {
        LOG_ERROR("[config] media_sources must be a list");
        return false;
    }
    out.clear();
    std::set<std::string> ids;
    std::set<std::string> enabled_app_streams;
    size_t enabled_count = 0;
    for (const auto &item : node) {
        if (!item || !item.IsMap()) {
            LOG_ERROR("[config] media_sources item must be a map");
            return false;
        }

        MediaSourceConfig src;
        src.id = YamlStringOrEmpty(item["id"]);
        if (src.id.empty()) {
            LOG_ERROR("[config] media_sources item needs a non-empty id");
            return false;
        }
        if (!ids.insert(src.id).second) {
            LOG_ERROR("[config] duplicate media_sources id: %s", src.id.c_str());
            return false;
        }

        if (item["enable"]) {
            src.enable = item["enable"].as<bool>();
        }

        src.type = YamlStringOrEmpty(item["type"]);
        if (src.type.empty()) src.type = "flv";
        for (char &c : src.type) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        if (src.type != "flv") {
            LOG_ERROR("[config] media_sources %s: unsupported type \"%s\" "
                      "(first version supports only flv)",
                      src.id.c_str(), src.type.c_str());
            return false;
        }

        src.app = YamlStringOrEmpty(item["app"]);
        src.stream = YamlStringOrEmpty(item["stream"]);
        if (src.app.empty() || src.stream.empty()) {
            LOG_ERROR("[config] media_sources %s: app and stream are required",
                      src.id.c_str());
            return false;
        }
        if (src.app.find('/') != std::string::npos ||
            src.stream.find('/') != std::string::npos) {
            LOG_ERROR("[config] media_sources %s: app/stream must not contain '/'",
                      src.id.c_str());
            return false;
        }
        const std::string stream_key = src.app + "/" + src.stream;
        /* disabled source 不会实际发布，因此不占用 app/stream。这样可以把
         * 同一路流的多个候选配置都留在文件里，只切换 enable。 */
        if (src.enable && !enabled_app_streams.insert(stream_key).second) {
            LOG_ERROR("[config] media_sources %s: duplicate enabled app/stream %s",
                      src.id.c_str(), stream_key.c_str());
            return false;
        }

        YAML::Node paths = item["paths"];
        if (!paths || !paths.IsSequence() || paths.size() == 0) {
            LOG_ERROR("[config] media_sources %s: paths must be a non-empty list",
                      src.id.c_str());
            return false;
        }
        for (const auto &path_node : paths) {
            std::string path = TrimCopy(path_node.as<std::string>());
            if (path.empty()) {
                LOG_ERROR("[config] media_sources %s: empty path", src.id.c_str());
                return false;
            }
            src.paths.push_back(std::move(path));
        }
        if (item["realtime"]) {
            src.realtime = item["realtime"].as<bool>();
        }
        if (item["loop"]) {
            src.loop = item["loop"].as<bool>();
        }
        if (src.enable) ++enabled_count;
        out.push_back(std::move(src));
    }
    LOG_INFO("[config] media_sources: total=%zu enabled=%zu disabled=%zu",
             out.size(), enabled_count, out.size() - enabled_count);
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

        YAML::Node wsflv = root["websocket_flv"];
        if (wsflv && wsflv.IsMap()) {
            if (wsflv["enable"]) {
                websocket_flv_enabled_ = wsflv["enable"].as<bool>();
            }
            if (wsflv["listenip"]) {
                websocket_flv_listen_ip_ = wsflv["listenip"].as<std::string>();
            }
            if (wsflv["port"]) {
                websocket_flv_port_ = wsflv["port"].as<int>();
            }
        }

        /* ── RTMP ──
         * 缺省关闭：不写这一节、或写了但不给 `enable: true`，都不监听。
         * 字段即使关闭也会被解析 —— 这样可以先提交配置，等 listener 接入
         * 之后只改一个 enable。 */
        YAML::Node rtmp = root["rtmp"];
        if (rtmp && rtmp.IsMap()) {
            if (rtmp["enable"]) {
                rtmp_enabled_ = rtmp["enable"].as<bool>();
            }
            if (rtmp["listenip"]) {
                rtmp_listen_ip_ = rtmp["listenip"].as<std::string>();
            }
            if (rtmp["port"]) {
                rtmp_port_ = rtmp["port"].as<int>();
            }
            if (rtmp["idle_timeout_ms"]) {
                rtmp_idle_timeout_ms_ = rtmp["idle_timeout_ms"].as<int>();
            }
        }

        /* ── Cluster（Edge / Center）──
         * 缺省 off：不写这一节 = 行为与单机服务端完全一致。
         * 字段即使 off 也解析，理由同 rtmp：先写好配置，再切模式。 */
        YAML::Node cluster = root["cluster"];
        if (cluster && cluster.IsMap()) {
            std::string mode = YamlStringOrEmpty(cluster["mode"]);
            for (char &c : mode) {
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
            if (mode.empty() || mode == "off" || mode == "disabled") {
                cluster_mode_ = ClusterMode::kOff;
            } else if (mode == "edge") {
                cluster_mode_ = ClusterMode::kEdge;
            } else if (mode == "center") {
                /* 中心节点不需要角色：它就是一个普通服务端。把这条写清楚，
                 * 比悄悄当成 off 好 —— 免得操作者以为还有别的开关。 */
                LOG_ERROR("[config] cluster.mode=center is not needed: the center "
                          "is a plain server (leave cluster.mode unset/off and "
                          "point each edge's center_address at it)");
                return false;
            } else {
                LOG_ERROR("[config] cluster.mode must be off/edge, got \"%s\"",
                          mode.c_str());
                return false;
            }
            if (cluster["protocol"]) {
                cluster_protocol_ = YamlStringOrEmpty(cluster["protocol"]);
                for (char &c : cluster_protocol_) {
                    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                }
            }
            if (cluster["center_address"]) {
                cluster_center_address_ = YamlStringOrEmpty(cluster["center_address"]);
            }
            if (cluster["center_path"]) {
                cluster_center_path_ = YamlStringOrEmpty(cluster["center_path"]);
                while (!cluster_center_path_.empty() &&
                       cluster_center_path_[0] == '/') {
                    cluster_center_path_.erase(cluster_center_path_.begin());
                }
            }
            if (cluster["reconnect_min_ms"]) {
                cluster_reconnect_min_ms_ = cluster["reconnect_min_ms"].as<int>();
            }
            if (cluster["reconnect_max_ms"]) {
                cluster_reconnect_max_ms_ = cluster["reconnect_max_ms"].as<int>();
            }
            if (cluster["subscribe_timeout_ms"]) {
                cluster_subscribe_timeout_ms_ = cluster["subscribe_timeout_ms"].as<int>();
            }
            if (cluster["pull_acquire_timeout_ms"]) {
                cluster_pull_acquire_timeout_ms_ = cluster["pull_acquire_timeout_ms"].as<int>();
            }
            if (cluster["pull_idle_timeout_ms"]) {
                cluster_pull_idle_timeout_ms_ = cluster["pull_idle_timeout_ms"].as<int>();
            }
            if (cluster["max_pull_connections"]) {
                cluster_max_pull_connections_ = cluster["max_pull_connections"].as<int>();
            }
            if (cluster["negative_cache_ms"]) {
                cluster_negative_cache_ms_ = cluster["negative_cache_ms"].as<int>();
            }
        }

        std::vector<MediaSourceConfig> parsed_sources;
        YAML::Node media_sources = root["media_sources"];
        if (media_sources) {
            if (!LoadMediaSourceList(media_sources, parsed_sources)) {
                return false;
            }
        }
        media_sources_ = std::move(parsed_sources);

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

    for (const auto &src : media_sources_) {
        LOG_INFO("[config] media source id=%s enabled=%d type=%s app=%s "
                 "stream=%s paths=%zu first=%s realtime=%d loop=%d",
                 src.id.c_str(), src.enable ? 1 : 0, src.type.c_str(),
                 src.app.c_str(), src.stream.c_str(), src.paths.size(),
                 src.paths[0].c_str(), src.realtime ? 1 : 0,
                 src.loop ? 1 : 0);
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
        cert_file_ = EnvOrEmpty("MOQ_SSL_CERT_FILE");
        key_file_ = EnvOrEmpty("MOQ_SSL_KEY_FILE");
    }
    if (cert_file_.empty() || key_file_.empty()) {
        ConfigFail("[config] certfile/keyfile are empty; set them in YAML "
                   "or export MOQ_SSL_CERT_FILE and MOQ_SSL_KEY_FILE "
                   "(legacy SSL_CERT_FILE/SSL_KEY_FILE are no longer used)");
        return false;
    }
    if (httpflv_listen_ip_.empty() || httpflv_port_ <= 0 || httpflv_port_ > 65535) {
        LOG_ERROR("[config] invalid httpflv listen %s:%d",
                  httpflv_listen_ip_.c_str(), httpflv_port_);
        return false;
    }

    if (websocket_flv_enabled_) {
        if (websocket_flv_listen_ip_.empty() ||
            websocket_flv_port_ <= 0 || websocket_flv_port_ > 65535) {
            LOG_ERROR("[config] invalid websocket_flv listen %s:%d",
                      websocket_flv_listen_ip_.c_str(), websocket_flv_port_);
            return false;
        }
        const bool same_ip = (websocket_flv_listen_ip_ == httpflv_listen_ip_);
        const bool wildcard = (websocket_flv_listen_ip_ == "0.0.0.0") ||
                              (httpflv_listen_ip_ == "0.0.0.0");
        if (websocket_flv_port_ == httpflv_port_ && (same_ip || wildcard)) {
            LOG_ERROR("[config] websocket_flv and httpflv both listen on tcp port %d",
                      websocket_flv_port_);
            return false;
        }
        if (rtmp_enabled_ && websocket_flv_port_ == rtmp_port_) {
            const bool rtmp_same = (websocket_flv_listen_ip_ == rtmp_listen_ip_);
            const bool rtmp_wild = (websocket_flv_listen_ip_ == "0.0.0.0") ||
                                   (rtmp_listen_ip_ == "0.0.0.0");
            if (rtmp_same || rtmp_wild) {
                LOG_ERROR("[config] websocket_flv and rtmp both listen on tcp port %d",
                          websocket_flv_port_);
                return false;
            }
        }
    }

    /* ── RTMP ──
     * 只在 enable: true 时校验监听参数。关闭时字段仍会解析，但写错不应拦住
     * 服务启动 —— 否则「先写好配置、下次再开」会变成「这次先起不来」。 */
    if (rtmp_enabled_) {
        if (rtmp_listen_ip_.empty() || rtmp_port_ <= 0 || rtmp_port_ > 65535) {
            LOG_ERROR("[config] invalid rtmp listen %s:%d",
                      rtmp_listen_ip_.c_str(), rtmp_port_);
            return false;
        }
        /* RTMP 和 HTTP-FLV 都是 TCP。同一地址 + 同一端口必然 bind 失败，
         * 这里提前报成配置错误；端口相同但地址不同时，只要有一边是
         * 0.0.0.0 也算冲突（通配地址会占住该端口的所有本地地址）。 */
        const bool same_ip = (rtmp_listen_ip_ == httpflv_listen_ip_);
        const bool wildcard = (rtmp_listen_ip_ == "0.0.0.0") ||
                              (httpflv_listen_ip_ == "0.0.0.0");
        if (rtmp_port_ == httpflv_port_ && (same_ip || wildcard)) {
            LOG_ERROR("[config] rtmp and httpflv both listen on tcp port %d "
                      "(rtmp=%s:%d httpflv=%s:%d)",
                      rtmp_port_, rtmp_listen_ip_.c_str(), rtmp_port_,
                      httpflv_listen_ip_.c_str(), httpflv_port_);
            return false;
        }
        if (rtmp_idle_timeout_ms_ < 1000) {
            LOG_WARN("[config] rtmp idle_timeout_ms=%d is too small, clamped to 1000",
                     rtmp_idle_timeout_ms_);
            rtmp_idle_timeout_ms_ = 1000;
        } else if (rtmp_idle_timeout_ms_ > 600000) {
            LOG_WARN("[config] rtmp idle_timeout_ms=%d is too large, clamped to 600000",
                     rtmp_idle_timeout_ms_);
            rtmp_idle_timeout_ms_ = 600000;
        }
    }

    /* ── Cluster ──
     * 只在 edge 时校验出站参数。center 不建出站连接，写错地址不该拦住启动。 */
    if (cluster_mode_ == ClusterMode::kEdge) {
        if (cluster_protocol_ != "moq") {
            ConfigFail("[config] cluster.protocol must be \"moq\" in phase 1 "
                       "(rtmp / moq-draft are not implemented)");
            return false;
        }
        if (cluster_center_address_.empty()) {
            ConfigFail("[config] cluster.center_address is required when mode=edge");
            return false;
        }
        /* center_address = host[:port]。IPv4 字面量或域名；本仓 QUIC 发送
         * 路径写死 IPv4，IPv6 字面量（含 ':' 的方括号形式）这里直接拒绝。 */
        const size_t colon = cluster_center_address_.find(':');
        if (cluster_center_address_.find('[', 0) != std::string::npos ||
            (colon != std::string::npos &&
             cluster_center_address_.find(':', colon + 1) != std::string::npos)) {
            ConfigFail("[config] cluster.center_address must be host[:port] (IPv4); "
                       "IPv6 is not supported");
            return false;
        }
        std::string host = cluster_center_address_;
        int port = 4433;
        if (colon != std::string::npos) {
            host = TrimCopy(cluster_center_address_.substr(0, colon));
            const std::string port_str = TrimCopy(cluster_center_address_.substr(colon + 1));
            char *end = nullptr;
            long v = port_str.empty() ? 0L : std::strtol(port_str.c_str(), &end, 10);
            if (port_str.empty() || !end || *end != '\0' || v <= 0 || v > 65535) {
                ConfigFail("[config] cluster.center_address has an invalid port");
                return false;
            }
            port = static_cast<int>(v);
        }
        if (host.empty()) {
            ConfigFail("[config] cluster.center_address has an empty host");
            return false;
        }
        cluster_center_address_ = host + ":" + std::to_string(port);
        if (cluster_center_path_.empty() ||
            cluster_center_path_.find('?') != std::string::npos ||
            cluster_center_path_.find('#') != std::string::npos) {
            ConfigFail("[config] cluster.center_path must be a non-empty path "
                       "without '?' or '#'");
            return false;
        }
    } else if (!cluster_center_address_.empty()) {
        LOG_WARN("[config] cluster.center_address is ignored unless mode=edge");
    }

    if (cluster_reconnect_min_ms_ < 100) cluster_reconnect_min_ms_ = 100;
    if (cluster_reconnect_max_ms_ < cluster_reconnect_min_ms_) {
        cluster_reconnect_max_ms_ = cluster_reconnect_min_ms_;
    }
    if (cluster_reconnect_max_ms_ > 600000) cluster_reconnect_max_ms_ = 600000;
    if (cluster_subscribe_timeout_ms_ < 100) cluster_subscribe_timeout_ms_ = 100;
    if (cluster_subscribe_timeout_ms_ > 60000) cluster_subscribe_timeout_ms_ = 60000;
    if (cluster_pull_acquire_timeout_ms_ < 100) cluster_pull_acquire_timeout_ms_ = 100;
    if (cluster_pull_acquire_timeout_ms_ > 60000) cluster_pull_acquire_timeout_ms_ = 60000;
    if (cluster_pull_idle_timeout_ms_ < 1000) cluster_pull_idle_timeout_ms_ = 1000;
    if (cluster_max_pull_connections_ < 1) cluster_max_pull_connections_ = 1;
    if (cluster_max_pull_connections_ > 4096) cluster_max_pull_connections_ = 4096;
    if (cluster_negative_cache_ms_ < 0) cluster_negative_cache_ms_ = 0;
    return true;
}

} /* namespace cpp_streamer */
