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
static int g_dgram_count = 0;

/* ── datagram 回调 — 回显数据报 ─────────────── */
static void on_datagram(QuicConnection *conn,
                         const uint8_t *data, size_t len) {
    LOG_INFO("[dgram-srv] received datagram: %zu bytes '%.*s'",
             len, (int)len, (const char*)data);
    g_dgram_count++;

    /* 回显 */
    int ret = QuicConnectionSendDatagram(conn, data, len);
    if (ret < 0)
        LOG_ERROR("[dgram-srv] echo send failed: %d", ret);
    else
        LOG_INFO("[dgram-srv] echoed %zu bytes", len);
}

static void on_stream_data(QuicConnection *conn, uint64_t stream_id,
                           const uint8_t *data, size_t len, int fin) {
    LOG_INFO("[dgram-srv] stream %llu: %zu bytes '%.*s' fin=%d",
             (unsigned long long)stream_id, len, (int)len,
             (const char*)data, fin);

    /* stream echo */
    QuicConnectionStreamSend(conn, stream_id, data, len, fin);
}

static void on_new_connection(QuicListener *l, QuicConnection *conn,
                              const char *remote_ip, int remote_port) {
    LOG_INFO("[dgram-srv] new connection from %s:%d", remote_ip, remote_port);
    QuicConnectionSetOnDatagram(conn, on_datagram);
    QuicConnectionSetOnStreamData(conn, on_stream_data);
}

static void on_signal(uv_signal_t *sig, int signum) {
    LOG_INFO("[dgram-srv] signal %d, shutting down", signum);
    uv_signal_stop(sig);
    if (g_listener) { QuicListenerDestruct(g_listener); g_listener = NULL; }
    uv_stop(sig->loop);
}

int main(int argc, char *argv[]) {
    setbuf(stdout, NULL); setbuf(stderr, NULL);

    const char *cert_file = NULL, *key_file = NULL, *ip = NULL;
    int port = 4444;
    for (int i = 1; i < argc; i++) {
        if (!cert_file) cert_file = argv[i];
        else if (!key_file) key_file = argv[i];
        else if (!ip) ip = argv[i];
        else port = atoi(argv[i]);
    }
    if (!cert_file) cert_file = "../cert/server_cert.pem";
    if (!key_file)  key_file  = "../cert/server_key.pem";
    if (!ip)        ip        = "127.0.0.1";

    uv_loop_t *loop = uv_default_loop();
    uv_signal_t sig;
    uv_signal_init(loop, &sig);
    uv_signal_start(&sig, on_signal, SIGINT);

    log_set_level(INFO);

    g_listener = QuicListenerCreate(loop, cert_file, key_file);
    if (!g_listener) { LOG_ERROR("[dgram-srv] create listener failed"); return 1; }
    QuicListenerSetOnConnection(g_listener, on_new_connection);

    int ret = QuicListenerListen(g_listener, ip, port);
    if (ret < 0) { LOG_ERROR("[dgram-srv] listen failed"); return 1; }
    LOG_INFO("[dgram-srv] listening on %s:%d (press Ctrl+C)", ip, port);

    uv_run(loop, UV_RUN_DEFAULT);

    if (g_listener) QuicListenerDestruct(g_listener);
    uv_close((uv_handle_t*)&sig, NULL);
    uv_run(loop, UV_RUN_NOWAIT);
    uv_loop_close(loop);
    quic_crypto_cleanup();
    log_shutdown();
    LOG_INFO("[dgram-srv] done, received %d datagrams", g_dgram_count);
    return 0;
}
