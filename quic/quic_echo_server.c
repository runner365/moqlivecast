#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uv.h>
#include "quic_listener.h"
#include "quic_connection.h"
#include "quic_crypto.h"
#include "tls_common.h"
#include "logger.h"

static QuicListener *g_listener = NULL;

static void on_stream_data(QuicConnection *conn, uint64_t stream_id,
                           const uint8_t *data, size_t len, int fin) {
    LOG_INFO("[server] received %zu bytes on stream %llu: %.*s",
             len, (unsigned long long)stream_id, (int)len, data);

    /* 回显数据 */
    int ret = QuicConnectionStreamSend(conn, stream_id, data, len, fin);
    if (ret < 0) {
        LOG_ERROR("[server] echo send failed on stream %llu",
                  (unsigned long long)stream_id);
        return;
    }
    LOG_INFO("[server] echoed %zu bytes on stream %llu (fin=%d)",
             len, (unsigned long long)stream_id, fin);

    if (fin) {
        QuicConnectionClose(conn, QUIC_ERR_NO_ERROR, "echo done");
    }
}

static void on_new_connection(QuicListener *l, QuicConnection *conn,
                              const char *remote_ip, int remote_port) {
    LOG_INFO("[server] new connection from %s:%d", remote_ip, remote_port);
    QuicConnectionSetOnStreamData(conn, on_stream_data);
}

static void on_signal(uv_signal_t *sig, int signum) {
    LOG_INFO("[server] signal %d, shutting down", signum);
    uv_signal_stop(sig);
    if (g_listener) {
        QuicListenerDestruct(g_listener);
        g_listener = NULL;
    }
    uv_stop(sig->loop);
}

int main(int argc, char *argv[]) {
    setbuf(stdout, NULL);
    setbuf(stderr, NULL);

    uv_loop_t *loop = uv_default_loop();

    /* 信号处理 */
    uv_signal_t sig;
    uv_signal_init(loop, &sig);
    uv_signal_start(&sig, on_signal, SIGINT);

    /* 扫描命令行参数中的 cipher hint */
    const char *cert_file = NULL;
    const char *key_file  = NULL;
    const char *ip        = NULL;
    int         port      = 4433;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--aes256") == 0)
            tls_set_cipher_hint("TLS_AES_256_GCM_SHA384");
        else if (strcmp(argv[i], "--chacha20") == 0)
            tls_set_cipher_hint("TLS_CHACHA20_POLY1305_SHA256");
        else if (!cert_file)
            cert_file = argv[i];
        else if (!key_file)
            key_file  = argv[i];
        else if (!ip)
            ip = argv[i];
        else
            port = atoi(argv[i]);
    }
    if (!cert_file) cert_file = "../cert/server_cert.pem";
    if (!key_file)  key_file  = "../cert/server_key.pem";
    if (!ip)        ip        = "127.0.0.1";

    log_set_level(INFO);
    
    g_listener = QuicListenerCreate(loop, cert_file, key_file);
    if (!g_listener) {
        LOG_ERROR("[server] create listener failed");
        return 1;
    }

    QuicListenerSetOnConnection(g_listener, on_new_connection);

    int ret = QuicListenerListen(g_listener, ip, port);
    if (ret < 0) {
        LOG_ERROR("[server] listen failed");
        return 1;
    }

    LOG_INFO("[server] listening on %s:%d, press Ctrl+C to stop", ip, port);

    uv_run(loop, UV_RUN_DEFAULT);

    /* 清理 */
    if (g_listener) QuicListenerDestruct(g_listener);
    uv_run(loop, UV_RUN_NOWAIT);

    uv_close((uv_handle_t*)&sig, NULL);
    uv_run(loop, UV_RUN_NOWAIT);

    uv_loop_close(loop);
    quic_crypto_cleanup();
    log_shutdown();
    return 0;
}
