#ifndef WEBTRANSPORT_H
#define WEBTRANSPORT_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================
 * WebTransport Session (RFC 9220)
 * ============================================ */

typedef struct webtransport_session webtransport_session;

typedef void (*wt_session_on_dgram_fn)(webtransport_session *s,
    const uint8_t *data, size_t len);

/* stream_data: is_uni=1 for uni, 0 for bidi. fin=1 when stream closed. */
typedef void (*wt_session_on_stream_fn)(webtransport_session *s,
    uint64_t stream_id, int is_uni,
    const uint8_t *data, size_t len, int fin);

/* 会话关闭（连接关闭 / 主动 close）时触发 */
typedef void (*wt_session_on_close_fn)(webtransport_session *s);

struct webtransport_session {
    uint64_t  session_id;
    void     *conn;
    uint64_t  connect_stream_id;
    void     *user_data;
    wt_session_on_dgram_fn  on_datagram;
    wt_session_on_stream_fn on_stream_data;
    wt_session_on_close_fn  on_close;
};

/* ── Session API ────────────────────────────── */

void webtransport_session_set_on_datagram(webtransport_session *s,
                                           wt_session_on_dgram_fn cb);
void webtransport_session_set_on_stream_data(webtransport_session *s,
                                              wt_session_on_stream_fn cb);
void webtransport_session_set_on_close(webtransport_session *s,
                                        wt_session_on_close_fn cb);
int  webtransport_session_send_datagram(webtransport_session *s,
                                         const uint8_t *data, size_t len);
int  webtransport_session_send_stream_data(webtransport_session *s,
         uint64_t stream_id, const uint8_t *data, size_t len, int fin);
void webtransport_session_close(webtransport_session *s);
uint64_t webtransport_session_get_id(const webtransport_session *s);

/* ── Session factory ───────────────────────── */
webtransport_session *webtransport_session_create(void *conn,
                                                    uint64_t stream_id);
void webtransport_session_destroy(webtransport_session *s);

/* ── Session registry (API layer internal) ─── */
void webtransport_registry_add(void *conn, webtransport_session *s);
void webtransport_registry_remove(void *conn);
webtransport_session *webtransport_registry_find(void *conn);

#ifdef __cplusplus
}
#endif

#endif /* WEBTRANSPORT_H */
