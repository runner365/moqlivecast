#ifndef WT2WS_CONFIG_HPP
#define WT2WS_CONFIG_HPP

#include <cstddef>
#include <string>

namespace cpp_streamer {
namespace wt2ws {

struct Wt2wsConfig {
    /* WebTransport listener */
    std::string listen_ip = "0.0.0.0";
    int listen_port = 5433;
    /* Empty = not provided on the command line.  Resolved after parsing:
     * --cert/--key > MOQ_SSL_CERT_FILE/MOQ_SSL_KEY_FILE > bundled cert/server_*.pem */
    std::string cert_file;
    std::string key_file;
    std::string cert_source; /* "cli", "env" or "bundled" (for logging) */
    std::string path = "/wt2ws";

    /* WebSocket backend (the proxy is the WS client) */
    std::string backend_host = "127.0.0.1";
    int backend_port = 9001;
    std::string backend_path = "/";
    bool backend_ssl = false;

    /* Limits */
    size_t max_frame = 4 * 1024 * 1024;       /* one message on the WT leg */
    size_t max_queue = 8 * 1024 * 1024;       /* pending bytes, each direction */
    int idle_timeout_ms = 300000;             /* 5 min without traffic */
    int max_sessions = 1024;
    int connect_timeout_ms = 5000;

    /* Logging */
    std::string log_file = "/tmp/wt2ws_proxy.log";
    bool console = true;
};

/* Parse argv.  On error returns false and fills *err. */
bool ParseWt2wsArgs(int argc, char **argv, Wt2wsConfig *cfg, std::string *err);

/* "ws://127.0.0.1:9001/echo" or "wss://host/path" */
bool ParseBackendUrl(const std::string &url, Wt2wsConfig *cfg, std::string *err);

} /* namespace wt2ws */
} /* namespace cpp_streamer */

#endif /* WT2WS_CONFIG_HPP */
