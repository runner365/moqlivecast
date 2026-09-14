#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uv.h>
#include "tls_server.h"
#include "logger.h"

static TlsServer *g_server  = NULL;
static uv_signal_t g_sig;

static void on_handshake_done(TlsServer *s, int status) {
    if (status == 0) {
        LOG_INFO("[server] TLS 1.3 handshake complete");
    } else {
        LOG_ERROR("[server] handshake failed");
    }
}

static void on_secret(TlsServer *s, uint32_t level, int direction,
                      const unsigned char *secret, size_t len) {
    (void)secret;
    LOG_INFO("[server] secret: level=%u dir=%s len=%zu",
           (unsigned)level, direction == 0 ? "read" : "write", len);
}

static void on_close(TlsServer *s) {
    (void)s;
    LOG_INFO("[server] closed");
    uv_close((uv_handle_t*)&g_sig, NULL);
}

static void on_signal(uv_signal_t *sig, int signum) {
    LOG_INFO("[server] signal %d, shutting down", signum);
    uv_signal_stop(sig);
    TlsServerDestruct(g_server);
    g_server = NULL;
}

int main(int argc, char *argv[]) {
    setbuf(stdout, NULL);  /* 禁用缓冲，便于调试 */
    setbuf(stderr, NULL);
    uv_loop_t *loop = uv_default_loop();

    uv_signal_init(loop, &g_sig);
    uv_signal_start(&g_sig, on_signal, SIGINT);

    g_server = TlsServerConstruct(loop, "../cert/server_cert.pem", "../cert/server_key.pem");
    if (!g_server) {
        LOG_ERROR("failed to create TLS server");
        return 1;
    }

    TlsServerSetOnHandshakeDone(g_server, on_handshake_done);
    TlsServerSetOnSecret(g_server, on_secret);
    TlsServerSetOnClose(g_server, on_close);

    int ret = TlsServerListen(g_server, "127.0.0.1", 12345);
    if (ret < 0) return 1;

    LOG_INFO("[server] press Ctrl+C to stop");

    uv_run(loop, UV_RUN_DEFAULT);

    /* 清理：触发异步关闭链 */
    if (g_server) {
        TlsServerDestruct(g_server);
        g_server = NULL;
    }
    /* 总是再跑一圈处理待处理的 close 回调 */
    uv_run(loop, UV_RUN_NOWAIT);

    uv_loop_close(loop);
    log_shutdown();
    return 0;
}
