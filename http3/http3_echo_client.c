#include "http3_frame.h"
#include "quic_connection.h"
#include "quic_crypto.h"
#include "logger.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uv.h>

static QuicConnection *g_conn = NULL;

static void on_signal(uv_signal_t *sig, int signum) {
    LOG_INFO("[h3-client] signal %d", signum);
    uv_stop(sig->loop);
}

static void on_stream_data(QuicConnection *conn, uint64_t stream_id,
                           const uint8_t *data, size_t len, int fin) {
    (void)stream_id; (void)fin;

    const uint8_t *hdr, *body;
    size_t hdr_len, body_len;
    int consumed = h3_frame_parse_headers(data, len, &hdr, &hdr_len);
    if (consumed > 0) {
        LOG_INFO("[h3-client] response: %.*s", (int)hdr_len, (const char*)hdr);
        data += consumed; len -= consumed;
    }
    if (len > 0 && h3_frame_parse_data(data, len, &body, &body_len) > 0) {
        LOG_INFO("[h3-client] body[%zu]: %.*s", body_len, (int)body_len, (const char*)body);
    }
    QuicConnectionClose(conn, 0, "done");
}

static void on_connected(QuicConnection *conn) {
    LOG_INFO("[h3-client] QUIC handshake complete");
    QuicConnectionSetOnStreamData(conn, on_stream_data);

    /* 发送 Client SETTINGS (uni stream + stream type 0x00) */
    {
        uint64_t csid = QuicConnectionStreamOpenUni(conn);
        if (csid != UINT64_MAX) {
            uint8_t ctrl_type = H3_STREAM_TYPE_CONTROL;
            uint64_t ids[]  = { H3_SETTING_QPACK_MAX_TABLE_CAPACITY,
                                 H3_SETTING_MAX_FIELD_SECTION_SIZE };
            uint64_t vals[] = { 4096, 16384 };
            uint8_t frm[256];
            int flen = h3_frame_write_settings(frm, sizeof(frm), ids, vals, 2);
            QuicConnectionStreamSend(conn, csid, &ctrl_type, 1, 0);
            if (flen > 0)
                QuicConnectionStreamSend(conn, csid, frm, (size_t)flen, 0);
            LOG_INFO("[h3-client] sent SETTINGS on uni stream %llu",
                     (unsigned long long)csid);
        }
    }

    /* 打开 bidi stream — 发送 HTTP 请求 */
    uint64_t sid = QuicConnectionStreamOpen(conn);
    if (sid == UINT64_MAX) { LOG_ERROR("[h3-client] open failed"); return; }

    char hdr_text[512];
    int hlen = snprintf(hdr_text, sizeof(hdr_text),
                        ":method GET\r\n:path /hello\r\n\r\n");
    uint8_t frame[1024];
    int flen = h3_frame_write_headers(frame, sizeof(frame),
                                       (const uint8_t*)hdr_text, (size_t)hlen);
    if (flen > 0) {
        QuicConnectionStreamSend(conn, sid, frame, (size_t)flen, 1);
        LOG_INFO("[h3-client] sent GET /hello");
    }
}

static void on_close(QuicConnection *conn, uint64_t ec, const char *reason) {
    LOG_INFO("[h3-client] closed: err=%llu reason=%s",
             (unsigned long long)ec, reason ? reason : "");
    g_conn = NULL;
    uv_stop(QuicConnectionGetLoop(conn));
}

int main(int argc, char *argv[]) {
    setbuf(stdout, NULL); setbuf(stderr, NULL);
    const char *ip   = argc > 1 ? argv[1] : "127.0.0.1";
    int         port = argc > 2 ? atoi(argv[2]) : 4433;

    uv_loop_t *loop = uv_default_loop();
    log_set_level(INFO);

    g_conn = QuicConnectionCreate(loop);
    if (!g_conn) { LOG_ERROR("[h3-client] create failed"); return 1; }
    QuicConnectionSetOnConnected(g_conn, on_connected);
    QuicConnectionSetOnClose(g_conn, on_close);

    int ret = QuicConnectionConnect(g_conn, ip, port);
    if (ret < 0) { LOG_ERROR("[h3-client] connect failed"); return 1; }

    uv_signal_t sig;
    uv_signal_init(loop, &sig);
    uv_signal_start(&sig, on_signal, SIGINT);

    uv_run(loop, UV_RUN_DEFAULT);

    uv_close((uv_handle_t*)&sig, NULL);
    if (g_conn) QuicConnectionDestruct(g_conn);
    uv_run(loop, UV_RUN_NOWAIT);
    uv_loop_close(loop);
    quic_crypto_cleanup();
    log_shutdown();
    return 0;
}
