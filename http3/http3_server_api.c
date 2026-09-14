#include "http3_server_api.h"
#include "http3_server.h"
#include "http3_frame.h"
#include "quic_connection.h"
#include "qpack.h"
#include "webtransport.h"
#include "logger.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ============================================
 * 内部结构
 * ============================================ */

typedef struct route_entry {
    http3_method          method;
    char                 *path;
    http3_handler_fn      callback;
    struct route_entry   *next;
} route_entry;

struct http3_server_api {
    uv_loop_t    *loop_;
    http3_server *h3_;
    route_entry  *routes_;
    http3_on_datagram_fn  on_datagram_;
    http3_on_wt_session_fn on_wt_session_;
};

/* ============================================
 * 辅助
 * ============================================ */

static http3_kv *kv_new(const char *key, int klen,
                         const char *val, int vlen) {
    http3_kv *kv = (http3_kv*)calloc(1, sizeof(*kv));
    if (!kv) return NULL;
    kv->key   = (char*)malloc((size_t)(klen + 1));
    kv->value = (char*)malloc((size_t)(vlen + 1));
    if (!kv->key || !kv->value) { free(kv->key); free(kv->value); free(kv); return NULL; }
    memcpy(kv->key,   key, (size_t)klen); kv->key[klen]   = '\0';
    memcpy(kv->value, val, (size_t)vlen); kv->value[vlen] = '\0';
    return kv;
}

static void kv_free_list(http3_kv *kv) {
    while (kv) {
        http3_kv *n = kv->next;
        free(kv->key); free(kv->value); free(kv);
        kv = n;
    }
}

const char *http3_kv_val(http3_kv *kv, const char *key) {
    for (http3_kv *p = kv; p; p = p->next)
        if (strcmp(p->key, key) == 0) return p->value;
    return NULL;
}

static http3_method parse_method(const char *s) {
    if (!s) return HTTP3_GET;
    if (strcmp(s, "GET")     == 0) return HTTP3_GET;
    if (strcmp(s, "POST")    == 0) return HTTP3_POST;
    if (strcmp(s, "PUT")     == 0) return HTTP3_PUT;
    if (strcmp(s, "DELETE")  == 0) return HTTP3_DELETE;
    if (strcmp(s, "HEAD")    == 0) return HTTP3_HEAD;
    if (strcmp(s, "OPTIONS") == 0) return HTTP3_OPTIONS;
    if (strcmp(s, "CONNECT") == 0) return HTTP3_CONNECT;
    return HTTP3_GET;
}

/* parse query string "k1=v1&k2=v2" into linked list */
static http3_kv *parse_query(const char *qs) {
    if (!qs || !*qs) return NULL;
    http3_kv *head = NULL, *tail = NULL;
    const char *p = qs;
    while (*p) {
        const char *ks = p;
        while (*p && *p != '=' && *p != '&') p++;
        int klen = (int)(p - ks);
        const char *vs = "";
        int vlen = 0;
        if (*p == '=') {
            p++;
            vs = p;
            while (*p && *p != '&') p++;
            vlen = (int)(p - vs);
        }
        http3_kv *kv = kv_new(ks, klen, vs, vlen);
        if (!kv) break;
        if (!head) head = tail = kv;
        else { tail->next = kv; tail = kv; }
        if (*p == '&') p++;
    }
    return head;
}

/* ── QPACK HEADERS → linked list ──────────── */
static http3_kv *parse_headers_qpack(const uint8_t *data, size_t len) {
    QpackHeaderField fields[QPACK_MAX_FIELDS];
    int n = qpack_decode(data, len, fields, QPACK_MAX_FIELDS);
    if (n <= 0) return NULL;

    http3_kv *head = NULL, *tail = NULL;
    for (int i = 0; i < n; i++) {
        if (!fields[i].name) continue;
        const char *k = (const char*)fields[i].name;
        const char *v = (fields[i].value && fields[i].value_len > 0)
                      ? (const char*)fields[i].value : "";
        http3_kv *kv = kv_new(k, (int)fields[i].name_len,
                               v, (int)fields[i].value_len);
        if (!kv) break;
        if (!head) head = tail = kv;
        else { tail->next = kv; tail = kv; }
    }
    return head;
}

/* ── QPACK 发送 HEADERS 帧 ────────────────── */
static int send_qpack_headers(void *conn, uint64_t stream_id,
                               int status_code) {
    uint8_t hdr[256];
    size_t pos = 0;
    /* QPACK prefix: required_insert_count=0, delta_base=0 */
    hdr[pos++] = 0x00;
    hdr[pos++] = 0x00;

    /* :status — static table index 26, name ref + literal value */
    char sval[8];
    int svlen = snprintf(sval, sizeof(sval), "%d", status_code);
    hdr[pos++] = 0x5F;  /* 01(NR)|0(N)|1(T)|prefix=15 */
    hdr[pos++] = 0x0B;  /* continuation: 26-15=11 */
    pos += (size_t)quic_varint_encode(hdr + pos, sizeof(hdr) - pos, (uint64_t)svlen);
    memcpy(hdr + pos, sval, (size_t)svlen); pos += (size_t)svlen;

    /* HEADERS frame wrapper */
    uint8_t frame[512];
    int flen = h3_frame_write_headers(frame, sizeof(frame), hdr, pos);
    if (flen < 0) return -1;

    return QuicConnectionStreamSend(
        (QuicConnection*)conn, stream_id, frame, (size_t)flen, 0);
}

static int send_data_frame(void *conn, uint64_t stream_id,
                            const char *data, int len, int fin) {
    uint8_t frame[65536 + 16];
    int flen = h3_frame_write_data(frame, sizeof(frame),
                                    (const uint8_t*)data, (size_t)len);
    if (flen < 0) return -1;
    return QuicConnectionStreamSend(
        (QuicConnection*)conn, stream_id, frame, (size_t)flen, fin);
}

/* ── WebTransport CONNECT 响应 (RFC 9220 §3.1) ──── */
static int send_wt_connect_response(void *conn, uint64_t stream_id) {
    uint8_t hdr[512] = {0};
    size_t pos = 0;

    /* QPACK prefix */
    hdr[pos++] = 0x00;  /* required_insert_count = 0 */
    hdr[pos++] = 0x00;  /* delta_base = 0 */

    /* :status 200 — Literal NameRef, static[26], value="200" */
    hdr[pos++] = 0x5F;  /* 01|N=0|T=1|prefix=15 */
    hdr[pos++] = 0x0B;  /* cont: 26-15=11 */
    hdr[pos++] = 0x03;  /* value length "200" = 3, 7-bit prefix */
    memcpy(hdr + pos, "200", 3); pos += 3;

    /* sec-webtransport-http3-draft-02: 1 */
    {
        const char *name = "sec-webtransport-http3-draft-02";
        uint64_t nlen = (uint64_t)strlen(name);  /* 37 */
        /* 001|N=0|H=0|prefix(3b) in first byte; QPACK int = LE continuation */
        if (nlen < 7) { hdr[pos++] = (uint8_t)(0x20 | nlen); }
        else {
            hdr[pos++] = 0x27;  /* 0x20|7 */
            uint64_t r = nlen - 7;  /* 30 */
            while (r >= 128) { hdr[pos++] = (uint8_t)((r & 0x7f) | 0x80); r >>= 7; }
            hdr[pos++] = (uint8_t)(r & 0x7f);  /* 0x1e */
        }
        memcpy(hdr + pos, name, (size_t)nlen); pos += (size_t)nlen;
        /* value "1": 7-bit prefix */
        hdr[pos++] = 0x01; hdr[pos++] = '1';
    }

    /* HEADERS frame wrapper */
    uint8_t frame[1024];
    int flen = h3_frame_write_headers(frame, sizeof(frame), hdr, pos);
    if (flen < 0) return -1;

    /* WT CONNECT response: send HEADERS, keep stream alive for session lifetime */
    return QuicConnectionStreamSend(
        (QuicConnection*)conn, stream_id, frame, (size_t)flen, 0);
}

/* ── response_write 回调 ───────────────────── */
static void resp_write(http3_response *resp, const char *data, int len) {
    if (!resp->_headers_sent) {
        send_qpack_headers(resp->_conn, resp->_stream_id, resp->status_code);
        resp->_headers_sent = 1;
    }
    send_data_frame(resp->_conn, resp->_stream_id, data, len, 0);
}

static void resp_send_datagram(http3_response *resp, const char *data, int len) {
    QuicConnectionSendDatagram(
        (QuicConnection*)resp->_conn,
        (const uint8_t*)data, (size_t)len);
}

/* ── 发送 404 响应 ─────────────────────────── */
static void send_404(void *conn, uint64_t stream_id) {
    const char *body = "404 Not Found\r\n";
    send_qpack_headers(conn, stream_id, 404);
    send_data_frame(conn, stream_id, body, (int)strlen(body), 1);
}

/* ============================================
 * Server 回调 — 来自 http3_server
 * ============================================ */

static void on_server_request(http3_server *h3,
                               QuicConnection *qc,
                               uint64_t stream_id,
                               const uint8_t *hdrs, size_t hdrs_len,
                               const uint8_t *body, size_t body_len) {
    http3_server_api *api = (http3_server_api*)http3_server_get_user_data(h3);
    if (!api) return;

    /* 1. 解析 headers */
    http3_kv *headers = parse_headers_qpack(hdrs, hdrs_len);
    if (!headers) {
        LOG_DEBUG("[h3-api] QPACK parse failed, %zu bytes: %.*s",
                 hdrs_len, (int)(hdrs_len < 100 ? hdrs_len : 100), hdrs);
        send_404(qc, stream_id); return;
    }

    /* 2. 提取 method + path */
    const char *method_str = http3_kv_val(headers, ":method");
    const char *full_path  = http3_kv_val(headers, ":path");
    const char *wt_proto   = http3_kv_val(headers, ":protocol");

    LOG_DEBUG("[h3-api] request: method=%s path=%s protocol=%s",
             method_str ? method_str : "(null)",
             full_path  ? full_path  : "(null)",
             wt_proto   ? wt_proto   : "(null)");

    http3_method method = parse_method(method_str);
    const char *path = (full_path && *full_path == '/') ? full_path : "/";

    /* ── WebTransport CONNECT 握手 (RFC 9220 §3.1) ──
     * Chrome 不显式编码 :method，识别 :protocol=webtransport* 即视为 CONNECT。
     * quic-go 使用 ":protocol=webtransport-h3"（与 Chrome 的 webtransport 不同）。 */
    if (wt_proto &&
        (strcmp(wt_proto, "webtransport") == 0 ||
         strncmp(wt_proto, "webtransport", 12) == 0)) {
            const char *auth = http3_kv_val(headers, ":authority");
            LOG_DEBUG("[h3-api] WebTransport CONNECT %s",
                     auth ? auth : "(unknown)");
            send_wt_connect_response(qc, stream_id);

            /* 创建 session，注册到 registry，记录 CONNECT stream */
            webtransport_session *session =
                webtransport_session_create(qc, stream_id);
            if (session) {
                webtransport_registry_add(qc, session);
                if (api->on_wt_session_)
                    api->on_wt_session_(api, session,
                        full_path ? full_path : "/");
            }

            /* WT 会话建立，不再重置恢复层。
             * PTO 有 ACK 复位机制，即使丢包也会自动重传。 */

            kv_free_list(headers);
            return;
    }

    /* 3. parse path / query */
    char path_only[512];
    const char *qs = NULL;
    {
        const char *qm = strchr(path, '?');
        if (qm) {
            size_t pl = (size_t)(qm - path);
            if (pl >= sizeof(path_only)) pl = sizeof(path_only) - 1;
            memcpy(path_only, path, pl);
            path_only[pl] = '\0';
            path  = path_only;
            qs    = qm + 1;
        }
    }
    http3_kv *query = parse_query(qs);

    /* 4. 构造 request */
    http3_request req;
    memset(&req, 0, sizeof(req));
    req.method    = method;
    req.path      = path;
    req.headers   = headers;
    req.query     = query;
    req.body      = (uint8_t*)body;
    req.body_len  = (int)body_len;
    req._conn     = qc;
    req._stream_id = stream_id;

    /* 5. 构造 response */
    http3_response resp;
    memset(&resp, 0, sizeof(resp));
    resp.status_code   = 200;
    resp.write         = resp_write;
    resp.send_datagram = resp_send_datagram;
    resp._conn         = qc;
    resp._stream_id    = stream_id;

    /* 6. 路由匹配 — :method 不存在时(如 curl/ngtcp2)仅按 path 匹配 */
    int handled = 0;
    for (route_entry *r = api->routes_; r; r = r->next) {
        int ok = (strcmp(r->path, path) == 0);
        if (ok && method_str) ok = (r->method == method);
        if (ok) {
            r->callback(&req, &resp);
            handled = 1;
            break;
        }
    }

    if (!handled) {
        send_404(qc, stream_id);
    } else if (resp._headers_sent) {
        /* 正常路径：handler 已调用 response_write，发送 final DATA(0) */
        send_data_frame(qc, stream_id, NULL, 0, 1);
    }

    /* 7. 清理 */
    kv_free_list(query);
    kv_free_list(headers);
}

/* ── Datagram dispatch ───────────────────────── */
static void api_on_dgram(void *user_data, void *conn,
                          const uint8_t *data, size_t len) {
    http3_server_api *api = (http3_server_api*)user_data;

    /* WT session takes priority */
    webtransport_session *wt = webtransport_registry_find(conn);
    if (wt && wt->on_datagram) {
        LOG_DEBUG("[h3-api] dgram → WT session %llu",
                 (unsigned long long)wt->session_id);
        wt->on_datagram(wt, data, len);
        return;
    }

    if (api->on_datagram_)
        api->on_datagram_(api, data, len);
}

/* ============================================
 * 公开 API
 * ============================================ */

http3_server_api *http3_server_api_create(
    uv_loop_t *loop, const char *cert_file, const char *key_file,
    const char *ip, int port) {
    http3_server_api *api = (http3_server_api*)calloc(1, sizeof(*api));
    if (!api) return NULL;
    api->loop_ = loop;

    api->h3_ = http3_server_create(loop, cert_file, key_file, on_server_request);
    if (!api->h3_) { free(api); return NULL; }
    http3_server_set_user_data(api->h3_, api);
    http3_server_set_on_datagram(api->h3_, api_on_dgram);

    int ret = http3_server_listen(api->h3_, ip, port);
    if (ret < 0) { http3_server_destroy(api->h3_); free(api); return NULL; }

    return api;
}

void http3_server_api_destroy(http3_server_api *api) {
    if (!api) return;
    /* 释放路由 */
    route_entry *r = api->routes_;
    while (r) {
        route_entry *n = r->next;
        free(r->path);
        free(r);
        r = n;
    }
    http3_server_destroy(api->h3_);
    free(api);
}

int http3_server_api_add_handler(http3_server_api *api,
                                  http3_method      method,
                                  const char       *path,
                                  http3_handler_fn  callback) {
    if (!api || !path || !callback) return -1;
    route_entry *e = (route_entry*)calloc(1, sizeof(*e));
    if (!e) return -1;
    e->method   = method;
    e->path     = strdup(path);
    e->callback = callback;
    e->next     = api->routes_;
    api->routes_ = e;
    return 0;
}

void http3_server_api_set_on_datagram(http3_server_api *srv,
                                       http3_on_datagram_fn cb) {
    if (srv) srv->on_datagram_ = cb;
}

void http3_server_api_set_on_wt_session(http3_server_api *srv,
                                         http3_on_wt_session_fn cb) {
    if (srv) srv->on_wt_session_ = cb;
}
