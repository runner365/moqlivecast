#include "webtransport_server_api.h"
#include "http3_server_api.h"
#include "webtransport.h"
#include "quic_connection.h"
#include "logger.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define WT_MAX_PATHS  16
#define WT_MAX_PARAMS 16

struct wt_session {
    wt_server_t          *srv;
    webtransport_session *ws;
    wt_stream_t          *streams;
    wt_path_callbacks_t   cb;
    void                 *user_data;
    char                  path[256];
    char   param_names[WT_MAX_PARAMS][64];
    char   param_values[WT_MAX_PARAMS][256];
    int    param_count;
};

struct wt_stream {
    wt_session_t *sess;
    uint64_t      stream_id;
    void         *user_data;
    wt_stream_t  *next;
    wt_session_on_stream_open_fn open_cb;
    void         *open_cb_user;
    int           header_sent;      /* WT 帧头是否已发送 */
    int           header_stripped;  /* WT 帧头是否已从入站数据剥离 */
    int           peer_initiated;   /* 是否由对端初始化的流 */
    int           is_uni;           /* 1 = 单向流（对端只能发，本端不能回写） */
    wt_stream_on_writable_fn on_writable;
    void         *on_writable_user;
};

struct wt_server {
    uv_loop_t        *loop;
    http3_server_api *h3api;
    struct { char path[128]; wt_path_callbacks_t cb; } paths[WT_MAX_PATHS];
    int  path_count;
    wt_path_callbacks_t default_cb;
};

/* single-server global — accessed by WT callbacks */
static wt_server_t *g_srv = NULL;

/* ── QUIC varint decoder ──────────────────────── */
static size_t quic_varint_read(const uint8_t *data, size_t len,
                                uint64_t *value) {
    if (len < 1) return 0;
    size_t vl;
    switch (data[0] & 0xc0) {
        case 0x00: vl = 1; break;
        case 0x40: vl = 2; break;
        case 0x80: vl = 4; break;
        default:   vl = 8; break;
    }
    if (vl > len) return 0;
    *value = data[0] & 0x3F;
    for (size_t i = 1; i < vl; i++)
        *value = (*value << 8) | data[i];
    return vl;
}

/* ── fwd ─────────────────────────────────────── */
static void on_internal_wt_stream(webtransport_session *ws,
                                   uint64_t sid, int is_uni,
                                   const uint8_t *data, size_t len, int fin);
static void on_internal_wt_close(webtransport_session *ws);
static void on_wt_session(http3_server_api *api, webtransport_session *ws,
                           const char *path);
static wt_stream_t *find_stream(wt_session_t *sess, uint64_t sid);
static void route_session(wt_server_t *srv, wt_session_t *sess);

/* ============================================
 * 公开 API — Session/Stream info
 * ============================================ */

const char *wt_session_get_path(wt_session_t *sess) {
    return sess ? sess->path : "/";
}
const char *wt_session_get_param(wt_session_t *sess, const char *name) {
    if (!sess || !name) return NULL;
    for (int i = 0; i < sess->param_count; i++)
        if (strcmp(sess->param_names[i], name) == 0)
            return sess->param_values[i];
    return NULL;
}
int wt_session_param_count(wt_session_t *sess) {
    return sess ? sess->param_count : 0;
}
int wt_session_param_at(wt_session_t *sess, int i,
                        const char **name, const char **value) {
    if (!sess || i < 0 || i >= sess->param_count) return -1;
    if (name) *name = sess->param_names[i];
    if (value) *value = sess->param_values[i];
    return 0;
}

/* ============================================
 * 公开 API — Server
 * ============================================ */

wt_server_t *wt_server_create(uv_loop_t *loop) {
    wt_server_t *srv = (wt_server_t*)calloc(1, sizeof(*srv));
    if (srv) { srv->loop = loop; g_srv = srv; }
    return srv;
}

void wt_server_destroy(wt_server_t *srv) {
    if (!srv) return;
    if (srv->h3api) http3_server_api_destroy(srv->h3api);
    g_srv = NULL;
    free(srv);
}

int wt_server_listen(wt_server_t *srv,
                      const char *cert_file, const char *key_file,
                      const char *ip, int port) {
    if (!srv) return -1;
    srv->h3api = http3_server_api_create(srv->loop, cert_file, key_file, ip, port);
    if (!srv->h3api) return -1;
    http3_server_api_set_on_wt_session(srv->h3api, on_wt_session);
    LOG_DEBUG("[wt-srv] listening on %s:%d", ip, port);
    return 0;
}

void wt_server_add_path(wt_server_t *srv, const char *path,
                         wt_path_callbacks_t *cb) {
    if (!srv || srv->path_count >= WT_MAX_PATHS || !path || !cb) return;
    snprintf(srv->paths[srv->path_count].path, sizeof(srv->paths[0].path), "%s", path);
    srv->paths[srv->path_count].cb = *cb;
    srv->path_count++;
}

int wt_server_add_http_handler(wt_server_t *srv,
                                http3_method       method,
                                const char        *path,
                                http3_handler_fn   callback) {
    if (!srv || !srv->h3api || !path || !callback) return -1;
    return http3_server_api_add_handler(srv->h3api, method, path, callback);
}

/* ============================================
 * 公开 API — Session
 * ============================================ */

void wt_session_set_user_data(wt_session_t *sess, void *data)
    { if (sess) sess->user_data = data; }
void *wt_session_get_user_data(wt_session_t *sess)
    { return sess ? sess->user_data : NULL; }

uint64_t wt_session_get_id(wt_session_t *sess) {
    if (!sess || !sess->ws) return 0;
    return webtransport_session_get_id(sess->ws);
}

void wt_session_close(wt_session_t *sess) {
    if (!sess || !sess->ws) return;
    webtransport_session_close(sess->ws);
}

int wt_session_get_quic_stats(wt_session_t *sess, QuicConnectionStats *out) {
    if (!sess || !sess->ws || !sess->ws->conn || !out) return -1;
    return QuicConnectionGetStats((QuicConnection*)sess->ws->conn, out);
}

void wt_session_open_stream(wt_session_t *sess,
                             wt_session_on_stream_open_fn cb, void *user) {
    if (!sess || !sess->ws) return;
    uint64_t sid = QuicConnectionStreamOpen((QuicConnection*)sess->ws->conn);
    if (sid == UINT64_MAX) return;
    wt_stream_t *st = (wt_stream_t*)calloc(1, sizeof(*st));
    st->sess = sess; st->stream_id = sid;
    st->open_cb = cb; st->open_cb_user = user;
    st->next = sess->streams; sess->streams = st;
    if (cb) cb(sess->srv, sess, st, user);
}

/* ============================================
 * 公开 API — Stream
 * ============================================ */

void wt_stream_set_user_data(wt_stream_t *st, void *data)
    { if (st) st->user_data = data; }
void *wt_stream_get_user_data(wt_stream_t *st)
    { return st ? st->user_data : NULL; }
wt_session_t *wt_stream_get_session(wt_stream_t *st)
    { return st ? st->sess : NULL; }
int wt_stream_is_uni(wt_stream_t *st)
    { return st ? st->is_uni : 0; }

wt_stream_t *wt_server_open_uni_stream(wt_session_t *sess) {
    if (!sess || !sess->ws) return NULL;

    /* 服务端需已从客户端收到 MAX_STREAMS_UNI 配额，否则 open 返回
     * UINT64_MAX。此时上层应稍后重试，而不是当作致命错误。 */
    QuicConnection *qc = (QuicConnection*)sess->ws->conn;
    if (!qc) return NULL;

    uint64_t sid = QuicConnectionStreamOpenUni(qc);
    if (sid == UINT64_MAX) {
        LOG_WARN("[wt-srv] open uni stream failed (no MAX_STREAMS_UNI credit?)");
        return NULL;
    }

    wt_stream_t *st = (wt_stream_t*)calloc(1, sizeof(*st));
    if (!st) return NULL;
    st->sess = sess;
    st->stream_id = sid;
    st->is_uni = 1;
    st->peer_initiated = 0;   /* 本端发起 —— 可以写，收包回调不会来 */
    st->next = sess->streams;
    sess->streams = st;

    LOG_INFO("[wt-srv] opened uni stream %llu", (unsigned long long)sid);
    return st;
}

static void wt_quic_writable_tramp(QuicStream *s, void *user) {
    (void)s;
    wt_stream_t *st = (wt_stream_t *)user;
    if (st && st->on_writable) st->on_writable(st, st->on_writable_user);
}

void wt_stream_set_on_writable(wt_stream_t *st, wt_stream_on_writable_fn cb, void *user) {
    if (!st) return;
    st->on_writable = cb;
    st->on_writable_user = user;
    if (!st->sess || !st->sess->ws || !st->sess->ws->conn) return;
    QuicConnectionStreamSetOnWritable((QuicConnection *)st->sess->ws->conn,
                                      st->stream_id, wt_quic_writable_tramp, st);
}

int wt_stream_write(wt_stream_t *st, const uint8_t *data, size_t len) {
    if (!st || !st->sess || !st->sess->ws) return -1;
    if (!data && len > 0) return -1;

    /* 对端发起的单向流本端不能回写 —— 发出去会触发对端
     * PROTOCOL_VIOLATION（RFC 9114 §6.1）。这里直接拒绝并提示，
     * 避免上层误用后表现为「对端莫名断连」。 */
    if (st->peer_initiated && st->is_uni) {
        LOG_WARN("[wt-srv] refuse write on peer-initiated uni stream %llu "
                 "(%zu bytes dropped)", (unsigned long long)st->stream_id, len);
        return -1;
    }

    /* WT 首包前缀：流类型 varint + session_id varint。
     *   双向流：0x41（WT STREAM 帧）
     *   单向流：0x54（WT 单向数据流类型，draft-ietf-webtrans-http3）
     * ⚠️ 0x54 必须按 varint 编码成 2 字节 0x40 0x54 —— 0x54 的高 2 位
     * 是 01，单字节写会被解析成 2 字节 varint，对端判不出流类型。 */
    if (!st->header_sent && !st->peer_initiated) {
        uint8_t hdr[4];
        size_t n = 0;
        const uint64_t stype = st->is_uni ? 0x54 : 0x41;
        if (stype < 64) {
            hdr[n++] = (uint8_t)stype;
        } else {
            hdr[n++] = (uint8_t)(0x40 | ((stype >> 8) & 0x3f));
            hdr[n++] = (uint8_t)(stype & 0xff);
        }
        hdr[n++] = 0x00;   /* session_id */
        size_t total = n + len;
        uint8_t *framed = (uint8_t*)malloc(total);
        if (!framed) return -1;
        memcpy(framed, hdr, n);
        if (len > 0) memcpy(framed + n, data, len);
        int r = webtransport_session_send_stream_data(st->sess->ws, st->stream_id,
                                                       framed, total, 0);
        free(framed);
        if (r == 0) {
            st->header_sent = 1;
        } else if (r == 1) {
            LOG_WARN("[wt-srv] stream %llu write congested, queued %zu bytes",
                     (unsigned long long)st->stream_id, total);
        } else {
            LOG_ERROR("[wt-srv] stream %llu write error, r=%d",
                     (unsigned long long)st->stream_id, r);
        }
        return r;
    }
    int r = webtransport_session_send_stream_data(st->sess->ws, st->stream_id,
                                                   data, len, 0);
    if (r == 0) {
        /* accepted */
    } else if (r == 1) {
        LOG_WARN("[wt-srv] stream %llu write congested, queued %zu bytes",
                 (unsigned long long)st->stream_id, len);
    } else {
        LOG_ERROR("[wt-srv] stream %llu write error, r=%d",
                 (unsigned long long)st->stream_id, r);
    }
    return r;
}

void wt_stream_close(wt_stream_t *st) {
    if (!st || !st->sess || !st->sess->ws) return;
    webtransport_session_send_stream_data(st->sess->ws, st->stream_id,
                                           NULL, 0, 1);
}

/* ============================================
 * 内部 — http3_server_api 回调
 * ============================================ */

static void on_wt_session(http3_server_api *api, webtransport_session *ws,
                           const char *path) {
    (void)api; (void)ws;
    if (!g_srv) return;

    wt_session_t *sess = (wt_session_t*)calloc(1, sizeof(*sess));
    sess->srv = g_srv;
    sess->ws  = ws;
    ws->user_data = sess;

    /* parse path + query */
    const char *p = path ? path : "/";
    const char *qm = strchr(p, '?');
    if (qm) {
        size_t pl = (size_t)(qm - p);
        snprintf(sess->path, sizeof(sess->path), "%.*s", (int)pl, p);
        const char *qp = qm + 1;
        while (*qp && sess->param_count < WT_MAX_PARAMS) {
            while (*qp == '&') qp++; /* 兼容 app=live&&stream=123456 */
            if (!*qp) break;
            const char *amp = strchr(qp, '&');
            const char *eq = strchr(qp, '=');
            if (!eq || (amp && amp < eq)) {
                qp = amp ? amp + 1 : qp + strlen(qp);
                continue;
            }
            int nl = (int)(eq - qp); if (nl > 63) nl = 63;
            if (nl <= 0) {
                qp = amp ? amp + 1 : eq + 1;
                continue;
            }
            snprintf(sess->param_names[sess->param_count], 64, "%.*s", nl, qp);
            const char *v = eq + 1;
            int vl = amp ? (int)(amp - v) : (int)strlen(v);
            if (vl > 255) vl = 255;
            snprintf(sess->param_values[sess->param_count], 256, "%.*s", vl, v);
            sess->param_count++;
            qp = amp ? amp + 1 : v + vl;
        }
    } else {
        snprintf(sess->path, sizeof(sess->path), "%s", p);
    }

    /* register stream callback on WT session */
    webtransport_session_set_on_stream_data(ws, on_internal_wt_stream);
    webtransport_session_set_on_close(ws, on_internal_wt_close);

    /* route */
    route_session(g_srv, sess);
}

static void on_internal_wt_close(webtransport_session *ws) {
    if (!ws || !g_srv) return;
    wt_session_t *sess = (wt_session_t*)ws->user_data;
    if (!sess) return;

    LOG_DEBUG("[wt-srv] session close path=%s", sess->path);

    /* 通知 path 级回调 */
    if (sess->cb.on_session_close)
        sess->cb.on_session_close(g_srv, sess, sess->cb.user_data);

    /* 释放 session 的 stream 链表 */
    wt_stream_t *st = sess->streams;
    while (st) {
        wt_stream_t *n = st->next;
        free(st);
        st = n;
    }
    free(sess);
}

static void on_internal_wt_stream(webtransport_session *ws,
                                   uint64_t sid, int is_uni,
                                   const uint8_t *data, size_t len, int fin) {
    (void)fin;
    if (!g_srv) return;
    wt_session_t *sess = (wt_session_t*)ws->user_data;
    if (!sess) return;

    /* find or create stream — must do BEFORE header strip */
    wt_stream_t *st = find_stream(sess, sid);
    if (!st) {
        st = (wt_stream_t*)calloc(1, sizeof(*st));
        st->sess = sess; st->stream_id = sid;
        st->next = sess->streams; sess->streams = st;
        st->peer_initiated = 1;  /* mark as peer-initiated stream */
        /* 单向流：对端只能发，本端不得回写。记录下来供上层决策，
         * 也供 wt_stream_write 拒绝非法回写（会触发 PROTOCOL_VIOLATION）。 */
        st->is_uni = is_uni;
    }

    /* Strip WebTransport STREAM frame header — once per stream.
     * Format: Frame Type (i) = 0x41..0x5f, Session ID (i).
     * Variant parsing can false-positive on data — guard with flag. */
    const uint8_t *payload = data;
    size_t payload_len = len;
    if (!st->header_stripped && len > 0) {
        uint64_t frame_type;
        size_t ft_len = quic_varint_read(data, len, &frame_type);
        if (ft_len > 0 && frame_type >= 0x41 && frame_type <= 0x5f) {
            uint64_t session_id;
            size_t sid_len = quic_varint_read(data + ft_len,
                                               len - ft_len, &session_id);
            if (sid_len > 0) {
                size_t header_len = ft_len + sid_len;
                payload     = data + header_len;
                payload_len = len - header_len;
                st->header_stripped = 1;
            }
        }
    }

    if (payload_len > 0 && sess->cb.on_stream_data)
        sess->cb.on_stream_data(g_srv, sess, st, payload, payload_len,
                                sess->cb.user_data);
}

/* ============================================
 * 内部 helper
 * ============================================ */

static wt_stream_t *find_stream(wt_session_t *sess, uint64_t sid) {
    for (wt_stream_t *s = sess->streams; s; s = s->next)
        if (s->stream_id == sid) return s;
    return NULL;
}

static void route_session(wt_server_t *srv, wt_session_t *sess) {
    for (int i = 0; i < srv->path_count; i++) {
        if (strcmp(srv->paths[i].path, sess->path) == 0) {
            sess->cb = srv->paths[i].cb;
            if (sess->cb.on_session)
                sess->cb.on_session(srv, sess, sess->cb.user_data);
            return;
        }
    }
    sess->cb = srv->default_cb;
    if (sess->cb.on_session)
        sess->cb.on_session(srv, sess, sess->cb.user_data);
}
