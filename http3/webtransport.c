#include "webtransport.h"
#include "quic_connection.h"
#include "logger.h"
#include <stdlib.h>
#include <string.h>

/* ── 单调递增 session ID ────────────────────── */
static uint64_t g_next_session_id = 1;

/* ============================================
 * 公开 API
 * ============================================ */

webtransport_session *webtransport_session_create(void *conn,
                                                    uint64_t stream_id) {
    webtransport_session *s = (webtransport_session*)calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->session_id        = g_next_session_id++;
    s->conn              = conn;
    s->connect_stream_id = stream_id;
    LOG_DEBUG("[webtransport] session %llu created (stream %llu)",
             (unsigned long long)s->session_id,
             (unsigned long long)stream_id);
    return s;
}

void webtransport_session_destroy(webtransport_session *s) {
    if (!s) return;
    LOG_DEBUG("[webtransport] session %llu destroyed",
             (unsigned long long)s->session_id);
    free(s);
}

void webtransport_session_set_on_datagram(webtransport_session *s,
                                           wt_session_on_dgram_fn cb) {
    if (s) s->on_datagram = cb;
}

void webtransport_session_set_on_stream_data(webtransport_session *s,
                                              wt_session_on_stream_fn cb) {
    if (s) s->on_stream_data = cb;
}

void webtransport_session_set_on_close(webtransport_session *s,
                                        wt_session_on_close_fn cb) {
    if (s) s->on_close = cb;
}

int webtransport_session_send_datagram(webtransport_session *s,
                                        const uint8_t *data, size_t len) {
    if (!s || !s->conn) return -1;
    return QuicConnectionSendDatagram(
        (QuicConnection*)s->conn, data, len);
}

int webtransport_session_send_stream_data(webtransport_session *s,
         uint64_t stream_id, const uint8_t *data, size_t len, int fin) {
    if (!s || !s->conn) return -1;
    /* 走 send_buf：GOP 回放可能超过初始 MAX_STREAM_DATA，
     * 旧的 StreamSend 会直接丢数据，拉流侧永远等不到 video_config。 */
    return QuicConnectionStreamSendEx(
        (QuicConnection*)s->conn, stream_id, data, len, fin, NULL, NULL, 0);
}

void webtransport_session_close(webtransport_session *s) {
    if (!s || !s->conn) return;
    QuicConnectionClose((QuicConnection*)s->conn,
                         QUIC_ERR_NO_ERROR, "session close");
}

uint64_t webtransport_session_get_id(const webtransport_session *s) {
    return s ? s->session_id : 0;
}

/* ── Simple connection → session registry ───── */
#define WT_MAX_SESSIONS 16
static struct { void *conn; webtransport_session *session; } wt_reg[WT_MAX_SESSIONS];
static int wt_reg_cnt;

void webtransport_registry_add(void *conn, webtransport_session *s) {
    if (wt_reg_cnt < WT_MAX_SESSIONS) {
        wt_reg[wt_reg_cnt].conn    = conn;
        wt_reg[wt_reg_cnt].session = s;
        wt_reg_cnt++;
    }
}

void webtransport_registry_remove(void *conn) {
    for (int i = 0; i < wt_reg_cnt; i++) {
        if (wt_reg[i].conn == conn) {
            wt_reg[i] = wt_reg[--wt_reg_cnt];
            return;
        }
    }
}

webtransport_session *webtransport_registry_find(void *conn) {
    for (int i = 0; i < wt_reg_cnt; i++)
        if (wt_reg[i].conn == conn) return wt_reg[i].session;
    return NULL;
}
