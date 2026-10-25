#include "wt2ws_config.hpp"

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace cpp_streamer {
namespace wt2ws {

namespace {

bool ParseHostPort(const std::string &in, std::string *host, int *port,
                   int default_port, std::string *err) {
    if (in.empty()) {
        *err = "empty address";
        return false;
    }
    const size_t colon = in.rfind(':');
    if (colon == std::string::npos) {
        *host = in;
        *port = default_port;
        return true;
    }
    *host = in.substr(0, colon);
    const std::string p = in.substr(colon + 1);
    char *end = nullptr;
    const long v = std::strtol(p.c_str(), &end, 10);
    if (*host == "" || p.empty() || !end || *end != '\0' || v <= 0 || v > 65535) {
        *err = "invalid host:port \"" + in + "\"";
        return false;
    }
    *port = static_cast<int>(v);
    return true;
}

std::string Trim(const std::string &in) {
    const size_t b = in.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    const size_t e = in.find_last_not_of(" \t\r\n");
    return in.substr(b, e - b + 1);
}

std::string EnvOrEmpty(const char *name) {
    const char *v = std::getenv(name);
    if (!v || !v[0]) return {};
    return Trim(v);
}

bool FileReadable(const std::string &path) {
    if (path.empty()) return false;
    std::ifstream f(path, std::ios::binary);
    return f.good();
}

/* Precedence mirrors moqlivecast: explicit flags, then the two environment
 * variables, then the bundled development certificate.  A half-configured
 * pair is an error rather than silently mixing sources. */
bool ResolveCertPaths(Wt2wsConfig *cfg, std::string *err) {
    const bool cli_cert = !cfg->cert_file.empty();
    const bool cli_key = !cfg->key_file.empty();
    if (cli_cert != cli_key) {
        *err = "give both --cert and --key, or neither";
        return false;
    }
    if (cli_cert) {
        cfg->cert_source = "cli";
    } else {
        cfg->cert_file = EnvOrEmpty("MOQ_SSL_CERT_FILE");
        cfg->key_file = EnvOrEmpty("MOQ_SSL_KEY_FILE");
        const bool env_cert = !cfg->cert_file.empty();
        const bool env_key = !cfg->key_file.empty();
        if (env_cert && env_key) {
            cfg->cert_source = "env";
        } else {
            /* A lone MOQ_SSL_CERT_FILE is a half-configured pair; fall back to
             * the bundled cert instead of mixing sources. */
            cfg->cert_file = "cert/server_cert.pem";
            cfg->key_file = "cert/server_key.pem";
            cfg->cert_source = (env_cert || env_key)
                                   ? "bundled (ignored incomplete MOQ_SSL_* env)"
                                   : "bundled";
        }
    }
    if (!FileReadable(cfg->cert_file)) {
        *err = "certificate not readable: " + cfg->cert_file +
               " (use --cert/--key or MOQ_SSL_CERT_FILE/MOQ_SSL_KEY_FILE)";
        return false;
    }
    if (!FileReadable(cfg->key_file)) {
        *err = "private key not readable: " + cfg->key_file +
               " (use --cert/--key or MOQ_SSL_KEY_FILE)";
        return false;
    }
    return true;
}

bool NeedsValue(int i, int argc, const char *flag, std::string *err) {
    if (i + 1 >= argc) {
        *err = std::string("missing value for ") + flag;
        return false;
    }
    return true;
}

} /* namespace */

bool ParseBackendUrl(const std::string &url, Wt2wsConfig *cfg, std::string *err) {
    std::string rest;
    int default_port = 80;
    if (url.rfind("ws://", 0) == 0) {
        rest = url.substr(5);
        cfg->backend_ssl = false;
        default_port = 80;
    } else if (url.rfind("wss://", 0) == 0) {
        rest = url.substr(6);
        cfg->backend_ssl = true;
        default_port = 443;
    } else {
        *err = "backend must start with ws:// or wss://";
        return false;
    }

    const size_t slash = rest.find('/');
    std::string hostport = rest;
    cfg->backend_path = "/";
    if (slash != std::string::npos) {
        hostport = rest.substr(0, slash);
        cfg->backend_path = rest.substr(slash);
    }
    if (hostport.empty()) {
        *err = "backend host is empty";
        return false;
    }
    return ParseHostPort(hostport, &cfg->backend_host, &cfg->backend_port,
                         default_port, err);
}

bool ParseWt2wsArgs(int argc, char **argv, Wt2wsConfig *cfg, std::string *err) {
    for (int i = 1; i < argc; i++) {
        const std::string a = argv[i];
        if (a == "--listen") {
            if (!NeedsValue(i, argc, "--listen", err)) return false;
            if (!ParseHostPort(argv[++i], &cfg->listen_ip, &cfg->listen_port,
                               5433, err)) {
                return false;
            }
        } else if (a == "--path") {
            if (!NeedsValue(i, argc, "--path", err)) return false;
            cfg->path = argv[++i];
            if (cfg->path.empty() || cfg->path[0] != '/') {
                *err = "--path must start with /";
                return false;
            }
        } else if (a == "--cert") {
            if (!NeedsValue(i, argc, "--cert", err)) return false;
            cfg->cert_file = argv[++i];
        } else if (a == "--key") {
            if (!NeedsValue(i, argc, "--key", err)) return false;
            cfg->key_file = argv[++i];
        } else if (a == "--backend") {
            if (!NeedsValue(i, argc, "--backend", err)) return false;
            if (!ParseBackendUrl(argv[++i], cfg, err)) return false;
        } else if (a == "--max-frame") {
            if (!NeedsValue(i, argc, "--max-frame", err)) return false;
            cfg->max_frame = static_cast<size_t>(std::strtoull(argv[++i], nullptr, 10));
        } else if (a == "--max-queue") {
            if (!NeedsValue(i, argc, "--max-queue", err)) return false;
            cfg->max_queue = static_cast<size_t>(std::strtoull(argv[++i], nullptr, 10));
        } else if (a == "--idle-ms") {
            if (!NeedsValue(i, argc, "--idle-ms", err)) return false;
            cfg->idle_timeout_ms = std::atoi(argv[++i]);
        } else if (a == "--max-sessions") {
            if (!NeedsValue(i, argc, "--max-sessions", err)) return false;
            cfg->max_sessions = std::atoi(argv[++i]);
        } else if (a == "--connect-timeout-ms") {
            if (!NeedsValue(i, argc, "--connect-timeout-ms", err)) return false;
            cfg->connect_timeout_ms = std::atoi(argv[++i]);
        } else if (a == "--log-file") {
            if (!NeedsValue(i, argc, "--log-file", err)) return false;
            cfg->log_file = argv[++i];
        } else if (a == "--console") {
            cfg->console = true;
        } else if (a == "--no-console") {
            cfg->console = false;
        } else if (a == "--help" || a == "-h") {
            *err = "help";
            return false;
        } else {
            *err = "unknown argument \"" + a + "\"";
            return false;
        }
    }

    if (cfg->max_frame < 16 || cfg->max_frame > 64 * 1024 * 1024) {
        *err = "--max-frame must be between 16 and 67108864";
        return false;
    }
    if (cfg->max_queue < cfg->max_frame) {
        *err = "--max-queue must be >= --max-frame";
        return false;
    }
    if (cfg->idle_timeout_ms <= 0) {
        *err = "--idle-ms must be > 0";
        return false;
    }
    if (cfg->max_sessions <= 0) {
        *err = "--max-sessions must be > 0";
        return false;
    }
    if (cfg->listen_port <= 0 || cfg->listen_port > 65535) {
        *err = "invalid --listen port";
        return false;
    }
    if (!ResolveCertPaths(cfg, err)) {
        return false;
    }
    return true;
}

} /* namespace wt2ws */
} /* namespace cpp_streamer */
