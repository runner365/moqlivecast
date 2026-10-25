#include "wt2ws_config.hpp"
#include "wt2ws_server.hpp"

#include "logger.h"
#include "timer.hpp"

#include <cstdio>
#include <cstring>
#include <string>

#include <uv.h>

namespace {

cpp_streamer::wt2ws::Wt2wsServer *g_server = nullptr;
uv_signal_t g_sig_int;
uv_signal_t g_sig_term;

void Usage(const char *argv0) {
    std::fprintf(stderr,
        "wt2ws_proxy - WebTransport <-> WebSocket relay\n"
        "\n"
        "  browser --WebTransport--> wt2ws_proxy --WebSocket--> backend\n"
        "\n"
        "Usage: %s [options]\n"
        "  --listen <ip:port>        WebTransport listener (default 0.0.0.0:5433)\n"
        "  --path <path>             WebTransport path (default /wt2ws)\n"
        "  --cert <pem>              TLS certificate (default: $MOQ_SSL_CERT_FILE,\n"
        "                             else cert/server_cert.pem)\n"
        "  --key <pem>               TLS private key (default: $MOQ_SSL_KEY_FILE,\n"
        "                             else cert/server_key.pem)\n"
        "  --backend <ws-url>        ws://host:port/path or wss://...\n"
        "  --max-frame <bytes>       max WT message size (default 4 MiB)\n"
        "  --max-queue <bytes>       max queued bytes per direction (default 8 MiB)\n"
        "  --idle-ms <ms>            close an idle session (default 300000)\n"
        "  --max-sessions <n>        concurrent session limit (default 1024)\n"
        "  --log-file <path>         log file (default /tmp/wt2ws_proxy.log)\n"
        "  --console / --no-console  duplicate logs to stderr (default console)\n"
        "  -h, --help                this text\n"
        "\n"
        "Framing on the WT leg: [u32 BE payload length][u8 type][payload],\n"
        "type 1 = text, 2 = binary.  The proxy never parses the payload.\n",
        argv0);
}

void OnSignal(uv_signal_t *sig, int signum) {
    LOG_WARN("[wt2ws] signal %d, shutting down", signum);
    if (g_server) g_server->Stop();
    uv_signal_stop(sig);
    uv_close(reinterpret_cast<uv_handle_t *>(sig), nullptr);
    uv_stop(sig->loop);
}

} /* namespace */

int main(int argc, char **argv) {
    cpp_streamer::wt2ws::Wt2wsConfig cfg;
    std::string err;
    if (!cpp_streamer::wt2ws::ParseWt2wsArgs(argc, argv, &cfg, &err)) {
        if (err == "help") {
            Usage(argv[0]);
            return 0;
        }
        std::fprintf(stderr, "wt2ws_proxy: %s\n", err.c_str());
        Usage(argv[0]);
        return 1;
    }

    setbuf(stdout, nullptr);
    log_set_file(cfg.log_file.c_str());
    log_set_console(cfg.console ? 1 : 0);
    log_set_level(INFO);
    LOG_INFO("[wt2ws] TLS cert source=%s cert=%s",
             cfg.cert_source.c_str(), cfg.cert_file.c_str());

    uv_loop_t *loop = uv_default_loop();
    /* WebSocketClient's timer/ping machinery needs the shared timer started. */
    cpp_streamer::StreamerTimerInitialize(loop, 100);

    uv_signal_init(loop, &g_sig_int);
    uv_signal_init(loop, &g_sig_term);
    uv_signal_start(&g_sig_int, OnSignal, SIGINT);
    uv_signal_start(&g_sig_term, OnSignal, SIGTERM);

    cpp_streamer::wt2ws::Wt2wsServer server(loop);
    g_server = &server;
    if (!server.Start(cfg, &err)) {
        LOG_ERROR("[wt2ws] start failed: %s", err.c_str());
        return 1;
    }

    LOG_INFO("[wt2ws] ready");
    uv_run(loop, UV_RUN_DEFAULT);

    server.Stop();
    g_server = nullptr;
    LOG_INFO("[wt2ws] exited");
    return 0;
}
