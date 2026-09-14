#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uv.h>
#include "tls_client.h"
#include "logger.h"

static TlsClient  *g_client = NULL;
static uv_signal_t g_sig;

static void on_handshake_done(TlsClient *c, int status) {
    if (status == 0) {
        LOG_INFO("[client] TLS 1.3 handshake complete");
    } else {
        LOG_ERROR("[client] handshake failed");
    }
    uv_stop(c->loop_);
}

static void on_secret(TlsClient *c, uint32_t level, int direction,
                      const unsigned char *secret, size_t len) {
    (void)secret;
    LOG_INFO("[client] secret: level=%u dir=%s len=%zu",
           (unsigned)level, direction == 0 ? "read" : "write", len);
}

static void on_close(TlsClient *c) {
    (void)c;
    LOG_INFO("[client] closed");
}

static void on_signal(uv_signal_t *sig, int signum) {
    LOG_INFO("[client] signal %d, shutting down", signum);
    uv_signal_stop(sig);
    if (g_client) {
        TlsClientDestruct(g_client);
        g_client = NULL;
    }
}

int main(int argc, char *argv[]) {
    setbuf(stdout, NULL);
    setbuf(stderr, NULL);
    uv_loop_t *loop = uv_default_loop();

    uv_signal_init(loop, &g_sig);
    uv_signal_start(&g_sig, on_signal, SIGINT);

    g_client = TlsClientConstruct(loop);
    if (!g_client) {
        LOG_ERROR("failed to create TLS client");
        return 1;
    }

    TlsClientSetOnHandshakeDone(g_client, on_handshake_done);
    TlsClientSetOnSecret(g_client, on_secret);
    TlsClientSetOnClose(g_client, on_close);

    int ret = TlsClientConnect(g_client, "127.0.0.1", 12345);
    if (ret < 0) return 1;

    uv_run(loop, UV_RUN_DEFAULT);

    /* 清理：触发异步关闭链 */
    if (g_client) {
        TlsClientDestruct(g_client);
        g_client = NULL;
    }
    /* 总是再跑一圈处理待处理的 close 回调 */
    uv_run(loop, UV_RUN_NOWAIT);

    uv_loop_close(loop);
    log_shutdown();
    return 0;
}
