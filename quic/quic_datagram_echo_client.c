#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uv.h>
#include "quic_connection.h"
#include "quic_crypto.h"
#include "tls_common.h"
#include "logger.h"

static QuicConnection *g_conn = NULL;
static int g_dgram_echoed = 0;
static int g_total_dgram = 0;
static const char *g_test_payload = "Hello DATAGRAM!";
static int g_test_len = 0;

/* ── datagram 回调 ──────────────────────────── */
static void on_datagram(QuicConnection *conn,
                         const uint8_t *data, size_t len) {
    LOG_INFO("[dgram-cli] received datagram: %zu bytes '%.*s'",
             len, (int)len, (const char*)data);
    g_dgram_echoed++;
    g_total_dgram++;

    /* 验证回显匹配 */
    if (len == (size_t)g_test_len &&
        memcmp(data, g_test_payload, (size_t)g_test_len) == 0) {
        LOG_INFO("[dgram-cli] datagram echo MATCH!");
    } else {
        LOG_ERROR("[dgram-cli] datagram echo MISMATCH! "
                  "expected=%d got=%zu", g_test_len, len);
    }

    /* 收到回显后关闭 */
    QuicConnectionClose(conn, QUIC_ERR_NO_ERROR, "test done");
}

static void on_stream_data(QuicConnection *conn, uint64_t stream_id,
                           const uint8_t *data, size_t len, int fin) {
    LOG_INFO("[dgram-cli] stream %llu: %zu bytes '%.*s' fin=%d",
             (unsigned long long)stream_id, len, (int)len,
             (const char*)data, fin);
}

static void on_connected(QuicConnection *conn) {
    LOG_INFO("[dgram-cli] QUIC handshake complete!");

    QuicConnectionSetOnDatagram(conn, on_datagram);
    QuicConnectionSetOnStreamData(conn, on_stream_data);

    /* 发送 datagram */
    g_test_len = (int)strlen(g_test_payload);
    int ret = QuicConnectionSendDatagram(conn,
        (const uint8_t*)g_test_payload, (size_t)g_test_len);
    if (ret < 0) {
        LOG_ERROR("[dgram-cli] send datagram failed: %d", ret);
        QuicConnectionClose(conn, QUIC_ERR_INTERNAL, "send failed");
        return;
    }
    LOG_INFO("[dgram-cli] sent datagram: %d bytes '%s'",
             g_test_len, g_test_payload);
    g_total_dgram++;

    /* 也发一条 stream（验证混合模式） */
    const char *sdata = "hello stream";
    uint64_t sid = QuicConnectionStreamOpen(conn);
    if (sid != UINT64_MAX)
        QuicConnectionStreamSend(conn, sid,
            (const uint8_t*)sdata, strlen(sdata), 1);
}

static void on_close(QuicConnection *conn, uint64_t error_code,
                     const char *reason) {
    LOG_INFO("[dgram-cli] connection closed: err=%llu reason=%s",
             (unsigned long long)error_code, reason ? reason : "");
    g_conn = NULL;
    uv_stop(QuicConnectionGetLoop(conn));
}

int main(int argc, char *argv[]) {
    setbuf(stdout, NULL); setbuf(stderr, NULL);

    const char *ip   = "127.0.0.1";
    int         port = 4444;
    if (argc >= 2) ip = argv[1];
    if (argc >= 3) port = atoi(argv[2]);

    uv_loop_t *loop = uv_default_loop();
    log_set_level(INFO);

    g_conn = QuicConnectionCreate(loop);
    if (!g_conn) { LOG_ERROR("[dgram-cli] create failed"); return 1; }

    QuicConnectionSetOnConnected(g_conn, on_connected);
    QuicConnectionSetOnClose(g_conn, on_close);

    LOG_INFO("[dgram-cli] connecting to %s:%d", ip, port);
    int ret = QuicConnectionConnect(g_conn, ip, port);
    if (ret < 0) { LOG_ERROR("[dgram-cli] connect failed"); return 1; }

    uv_run(loop, UV_RUN_DEFAULT);

    if (g_conn) QuicConnectionDestruct(g_conn);
    uv_loop_close(loop);
    quic_crypto_cleanup();
    log_shutdown();

    if (g_dgram_echoed == 1 && g_total_dgram == 2)
        LOG_INFO("[dgram-cli] TEST PASSED: 1/1 datagram echoed");
    else
        LOG_ERROR("[dgram-cli] TEST FAILED: echoed=%d/%d",
                  g_dgram_echoed, g_total_dgram);

    return (g_dgram_echoed == 1) ? 0 : 1;
}
