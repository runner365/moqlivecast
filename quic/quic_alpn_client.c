/* 裸 QUIC 客户端的 ALPN 验证程序。
 *
 * 存在意义：moqlivecast 同一个 UDP 端口上跑两套协议，靠握手 ALPN 分流
 *   moq-lite-06 → 裸 QUIC（moq-cli / libmoq / FFmpeg）
 *   h3          → WebTransport（浏览器）
 * 而本 API 的客户端 ALPN 曾经是写死的 (h3 + quic-echo)（tls_common.h 的
 * TLS_ALPN_PROTO）—— 两边都 offer h3 时协商落到 h3，服务端于是把裸 QUIC
 * 客户端当成 WebTransport 等 CONNECT，**看起来连上了却没有任何下文**。
 * 这个程序就是钉住这条的回归：设成 moq-lite-06 后，客户端侧读回的协商结果
 * 必须是 moq-lite-06，服务端也必须走 moq-lite 分支。
 *
 * 用法：
 *   ./quic_alpn_client [ip] [port] [alpn...]
 * 默认  127.0.0.1 4433 moq-lite-06
 * 反例：./quic_alpn_client 127.0.0.1 4433 h3        （期望协商成 h3，走 WT）
 *
 * 退出码：0 = 协商结果与期望一致；1 = 不一致或连接失败。 */
#include "quic_connection.h"
#include "logger.h"
#include "quic_crypto.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static QuicConnection *g_conn = NULL;
static char            g_want[64] = {0};
static int             g_ok = 0;

static void on_connected(QuicConnection *conn) {
    /* 握手完成才读得到协商结果。空串 = 对端没选（或没提供）。 */
    const char *got = QuicConnectionGetAlpn(conn);
    char ip[64] = {0};
    int  port = 0;
    QuicConnectionGetPeer(conn, ip, sizeof(ip), &port);

    LOG_INFO("[alpn-cli] connected peer=%s:%d negotiated_alpn=\"%s\" want=\"%s\"",
             ip, port, got[0] ? got : "(none)", g_want);

    if (strcmp(got, g_want) == 0) {
        g_ok = 1;
        LOG_INFO("[alpn-cli] ALPN OK: negotiated \"%s\"", got);
    } else {
        g_ok = 0;
        LOG_ERROR("[alpn-cli] ALPN MISMATCH: got \"%s\", want \"%s\"",
                  got[0] ? got : "(none)", g_want);
    }

    /* 协商对了就走人：这个程序的唯一目的就是握手 + 验 ALPN。
     * 不主动关 —— 让服务端那边也能看到一次干净的握手记录。 */
    uv_stop(QuicConnectionGetLoop(conn));
}

static void on_close(QuicConnection *conn, uint64_t error_code,
                     const char *reason) {
    LOG_INFO("[alpn-cli] connection closed: err=0x%llx reason=%s",
             (unsigned long long)error_code, reason ? reason : "");
    g_conn = NULL;
    uv_stop(QuicConnectionGetLoop(conn));
}

int main(int argc, char *argv[]) {
    setbuf(stdout, NULL);
    setbuf(stderr, NULL);

    const char *ip   = "127.0.0.1";
    int         port = 4433;
    if (argc >= 2) ip   = argv[1];
    if (argc >= 3) port = atoi(argv[2]);

    /* 默认就 offer moq-lite-06 —— 这正是这条回归要验的。 */
    const char *protos[8];
    int         n = 0;
    if (argc >= 4) {
        for (int i = 3; i < argc && n < 8; i++) protos[n++] = argv[i];
    } else {
        protos[n++] = "moq-lite-06";
    }
    snprintf(g_want, sizeof(g_want), "%s", protos[0]);

    uv_loop_t *loop = uv_default_loop();
    log_set_level(INFO);

    g_conn = QuicConnectionCreate(loop);
    if (!g_conn) { LOG_ERROR("[alpn-cli] create failed"); return 1; }

    QuicConnectionSetOnConnected(g_conn, on_connected);
    QuicConnectionSetOnClose(g_conn, on_close);

    /* ⚠️ 必须在 Connect 之前 —— 它设的是已由 SSL_new 建好的 SSL 对象，
     * 握手一发起 offer 就发出去了。 */
    if (QuicConnectionSetAlpnList(g_conn, protos, n) < 0) {
        LOG_ERROR("[alpn-cli] SetAlpnList failed");
        QuicConnectionDestruct(g_conn);
        return 1;
    }

    LOG_INFO("[alpn-cli] connecting to %s:%d (offer: %s)", ip, port, g_want);

    if (QuicConnectionConnect(g_conn, ip, port) < 0) {
        LOG_ERROR("[alpn-cli] connect failed");
        QuicConnectionDestruct(g_conn);
        return 1;
    }

    uv_run(loop, UV_RUN_DEFAULT);

    if (g_conn) QuicConnectionDestruct(g_conn);
    uv_loop_close(loop);
    quic_crypto_cleanup();
    log_shutdown();

    if (g_ok) {
        LOG_INFO("[alpn-cli] TEST PASSED: negotiated \"%s\"", g_want);
        return 0;
    }
    LOG_ERROR("[alpn-cli] TEST FAILED: did not negotiate \"%s\"", g_want);
    return 1;
}
