#include "http3_server.h"
#include "http3_frame.h"
#include "webtransport.h"
#include "logger.h"
#include <stdlib.h>
#include <string.h>

#define MAX_H3_CONNS 64
#define MAX_WT_UNI   128

typedef struct {
    QuicConnection *conn;
    uint64_t        ctrl_stream_id;   /* 服务端→客户端控制流的 uni stream ID */
    int             goaway_sent;
    uint64_t        wt_uni[MAX_WT_UNI];
    int             wt_uni_n;
} H3Conn;

struct http3_server {
    uv_loop_t        *loop_;
    QuicListener     *listener_;
    http3_on_request   on_req_;
    H3Conn            conns_[MAX_H3_CONNS];
    int               conn_cnt_;
    void             *app_data_;
    void             *user_data_;
    http3_server_on_dgram_fn on_dgram_;
};

/* ── 连接管理 ─────────────────────────────── */

static H3Conn* h3_find_conn(http3_server *srv, QuicConnection *conn) {
    for (int i = 0; i < srv->conn_cnt_; i++)
        if (srv->conns_[i].conn == conn) return &srv->conns_[i];
    return NULL;
}

static void h3_add_conn(http3_server *srv, QuicConnection *conn) {
    if (srv->conn_cnt_ >= MAX_H3_CONNS) return;
    H3Conn *hc = &srv->conns_[srv->conn_cnt_];
    memset(hc, 0, sizeof(*hc));
    hc->conn = conn;
    srv->conn_cnt_++;
}

static int h3_is_wt_uni(const H3Conn *hc, uint64_t sid) {
    if (!hc) return 0;
    for (int i = 0; i < hc->wt_uni_n; i++)
        if (hc->wt_uni[i] == sid) return 1;
    return 0;
}

static void h3_remember_wt_uni(H3Conn *hc, uint64_t sid) {
    if (!hc || h3_is_wt_uni(hc, sid)) return;
    if (hc->wt_uni_n < MAX_WT_UNI) {
        hc->wt_uni[hc->wt_uni_n++] = sid;
        return;
    }
    memmove(hc->wt_uni, hc->wt_uni + 1, (MAX_WT_UNI - 1) * sizeof(uint64_t));
    hc->wt_uni[MAX_WT_UNI - 1] = sid;
}

static void h3_forget_wt_uni(H3Conn *hc, uint64_t sid) {
    if (!hc) return;
    for (int i = 0; i < hc->wt_uni_n; i++) {
        if (hc->wt_uni[i] == sid) {
            hc->wt_uni[i] = hc->wt_uni[--hc->wt_uni_n];
            return;
        }
    }
}

static void h3_remove_conn(http3_server *srv, QuicConnection *conn) {
    for (int i = 0; i < srv->conn_cnt_; i++) {
        if (srv->conns_[i].conn == conn) {
            srv->conns_[i] = srv->conns_[--srv->conn_cnt_];
            return;
        }
    }
}

/* ============================================
 * 辅助 — 发送 Server SETTINGS
 * ============================================ */

static void send_server_settings(http3_server *srv, QuicConnection *conn) {
    (void)srv;

    uint64_t sid = QuicConnectionStreamOpenUni(conn);
    if (sid == UINT64_MAX) {
        LOG_WARN("[h3-server] failed to open control stream");
        return;
    }

    /* stream type prefix = control (0x00) */
    uint8_t ctrl_type = H3_STREAM_TYPE_CONTROL;

    /* 同时发送 draft 值和 RFC 9220 最终值，兼容 Chrome 和 quic-go */
    uint64_t ids[]   = { H3_SETTING_QPACK_MAX_TABLE_CAPACITY,
                          H3_SETTING_MAX_FIELD_SECTION_SIZE,
                          H3_SETTING_ENABLE_CONNECT_PROTOCOL,
                          H3_SETTING_ENABLE_WEBTRANSPORT_DRAFT02,
                          H3_SETTING_ENABLE_WEBTRANSPORT,
                          H3_SETTING_H3_DATAGRAM };
    uint64_t vals[]  = { 4096, 16384, 1, 1, 1, 1 };
    uint8_t  frm[256];
    int flen = h3_frame_write_settings(frm, sizeof(frm), ids, vals, 6);
    if (flen < 0) return;

    /* 单次 send: stream type + SETTINGS 在同一个 STREAM 帧中 */
    {
        uint8_t combined[300];
        combined[0] = ctrl_type;
        memcpy(combined + 1, frm, (size_t)flen);
        size_t total = (size_t)(1 + flen);
        /* hex dump for debugging */
        char hex[256]; size_t hp = 0;
        for (size_t h = 0; h < total && h < 64 && hp < sizeof(hex)-3; h++)
            hp += snprintf(hex+hp, sizeof(hex)-hp, "%02x", combined[h]);
        LOG_DEBUG("[h3-server] control stream %llu: stream_type+SETTINGS %zu bytes: %s",
                 (unsigned long long)sid, total, hex);
        int sret = QuicConnectionStreamSend(conn, sid, combined, total, 0);
        LOG_DEBUG("[h3-server] SETTINGS send sid=%llu len=%zu ret=%d",
                 (unsigned long long)sid, total, sret);
    }

    H3Conn *hc = h3_find_conn(srv, conn);
    if (hc) hc->ctrl_stream_id = sid;
}

/* ============================================
 * Stream 回调
 * ============================================ */

static void on_stream_data(QuicConnection *conn, uint64_t stream_id,
                           const uint8_t *data, size_t len, int fin) {
    http3_server *srv = (http3_server*)QuicConnectionGetAppData(conn);

    /* WT session: client bidi (4,8,12,…) and client uni (2,6,10,… type 0x54).
     * stream 0 is CONNECT. H3 control / QPACK also use client uni. */
    webtransport_session *wt = webtransport_registry_find(conn);
    H3Conn *hc = h3_find_conn(srv, conn);
    if (wt && stream_id > 0 && stream_id % 4 == 0 && wt->on_stream_data) {
        wt->on_stream_data(wt, stream_id, 0 /* bidi */, data, len, fin);
        return;
    }

    /* Uni stream（客户端→服务端）: stream ID 2,6,10,… */
    if (stream_id % 4 == 2) {
        if (wt && wt->on_stream_data) {
            int is_wt = h3_is_wt_uni(hc, stream_id);
            if (!is_wt && len > 0) {
                uint64_t stype = 0;
                if (quic_varint_decode(data, len, &stype) > 0 &&
                    stype == H3_STREAM_TYPE_WEBTRANSPORT) {
                    h3_remember_wt_uni(hc, stream_id);
                    is_wt = 1;
                    LOG_INFO("[h3-server] WT uni stream=%llu %zuB",
                             (unsigned long long)stream_id, len);
                }
            }
            if (is_wt) {
                wt->on_stream_data(wt, stream_id, 1 /* uni */, data, len, fin);
                if (fin) h3_forget_wt_uni(hc, stream_id);
                return;
            }
        }
        if (len < 1) return;
        uint8_t stype = data[0];
        data++; len--;
        if (stype == H3_STREAM_TYPE_CONTROL) {
            /* 解析控制帧 — SETTINGS 或 GOAWAY */
            uint64_t ids[8], vals[8];
            int count;
            int consumed = h3_frame_parse_settings(data, len,
                                                    ids, vals, 8, &count);
            if (consumed > 0) {
                for (int i = 0; i < count; i++) {
                    LOG_DEBUG("[h3-server] peer SETTING %llu=%llu",
                             (unsigned long long)ids[i], (unsigned long long)vals[i]);
                    if ((ids[i] == H3_SETTING_ENABLE_WEBTRANSPORT ||
                         ids[i] == H3_SETTING_ENABLE_WEBTRANSPORT_DRAFT02) &&
                        vals[i] == 1)
                        LOG_DEBUG("[h3-server] client supports WebTransport (id=0x%llx)",
                                 (unsigned long long)ids[i]);
                }
            } else {
                uint64_t last_id;
                consumed = h3_frame_parse_goaway(data, len, &last_id);
                if (consumed > 0)
                    LOG_DEBUG("[h3-server] peer GOAWAY last_stream_id=%llu",
                             (unsigned long long)last_id);
            }
        } else if (stype != H3_STREAM_TYPE_QPACK_ENC &&
                   stype != H3_STREAM_TYPE_QPACK_DEC) {
            LOG_INFO("[h3-server] client uni stream=%llu type=0x%02x %zuB (not WT 0x54)",
                     (unsigned long long)stream_id, stype, len + 1);
        }
        return;
    }

    /* Bidi stream — HTTP 请求 */
    LOG_DEBUG("[h3-server] stream %llu: %zu bytes", (unsigned long long)stream_id, len);

    const uint8_t *hdata;
    size_t hlen;
    int consumed = h3_frame_parse_headers(data, len, &hdata, &hlen);
    if (consumed <= 0) return;

    data += consumed; len -= consumed;

    const uint8_t *ddata = NULL;
    size_t dlen = 0;
    if (len > 0) {
        int dc = h3_frame_parse_data(data, len, &ddata, &dlen);
        if (dc <= 0) { ddata = data; dlen = len; }
    }

    if (srv->on_req_)
        srv->on_req_(srv, conn, stream_id,
                     hdata, hlen, ddata ? ddata : (const uint8_t*)"", dlen);
}

/* ============================================
 * 握手完成后回调 — 发送 H3 控制流
 * ============================================ */

static void on_handshake_done(QuicConnection *conn) {
    http3_server *srv = (http3_server*)QuicConnectionGetAppData(conn);
    if (!srv) return;

    LOG_DEBUG("[h3-server] HANDSHAKE_DONE callback fired — sending SETTINGS");

    /* HTTP/3 §6.2.1: 控制流必须是服务端第一个单向流。
     * QPACK 编码器/解码器流必须在控制流之后打开，
     * 否则 quic-go 等客户端会因流类型不匹配而忽略 SETTINGS。 */
    send_server_settings(srv, conn);

    /* QPACK encoder stream (uni, type=0x02) */
    {
        uint64_t esid = QuicConnectionStreamOpenUni(conn);
        if (esid != UINT64_MAX) {
            uint8_t qpack_enc = H3_STREAM_TYPE_QPACK_ENC;
            QuicConnectionStreamSend(conn, esid, &qpack_enc, 1, 0);
        }
    }
    /* QPACK decoder stream (uni, type=0x03) */
    {
        uint64_t dsid = QuicConnectionStreamOpenUni(conn);
        if (dsid != UINT64_MAX) {
            uint8_t qpack_dec = H3_STREAM_TYPE_QPACK_DEC;
            QuicConnectionStreamSend(conn, dsid, &qpack_dec, 1, 0);
        }
    }
}

/* ============================================
 * 连接回调
 * ============================================ */

/* ── QUIC Datagram → API 层转发 ─────────────── */
static void on_quic_datagram(QuicConnection *conn,
                              const uint8_t *data, size_t len) {
    http3_server *srv = (http3_server*)QuicConnectionGetAppData(conn);
    if (srv->on_dgram_)
        srv->on_dgram_(srv->user_data_, conn, data, len);
}

/* ── 链式关闭回调 ─────────────────────────── */
static void h3_on_close(QuicConnection *conn, uint64_t error_code,
                         const char *reason) {
    http3_server *srv = (http3_server*)QuicConnectionGetAppData(conn);
    /* 触发 WT session 关闭回调（供上层 roomManager 清理成员） */
    webtransport_session *ws = webtransport_registry_find(conn);
    if (ws && ws->on_close)
        ws->on_close(ws);
    webtransport_registry_remove(conn);
    if (srv) h3_remove_conn(srv, conn);
    /* 同时也从 listener 的连接列表清理 */
    void *listener = QuicConnectionGetListener(conn);
    if (listener) QuicListenerRemoveConn((QuicListener*)listener, conn);
    LOG_DEBUG("[h3-server] connection closed: err=%llu", (unsigned long long)error_code);
}

static void on_connection(QuicListener *l, QuicConnection *conn,
                          const char *remote_ip, int remote_port) {
    http3_server *srv = (http3_server*)QuicListenerGetAppData(l);
    LOG_DEBUG("[h3-server] new connection from %s:%d", remote_ip, remote_port);
    QuicConnectionSetAppData(conn, srv);
    QuicConnectionSetOnStreamData(conn, on_stream_data);
    QuicConnectionSetOnConnected(conn, on_handshake_done);
    QuicConnectionSetOnDatagram(conn, on_quic_datagram);
    QuicConnectionSetOnClose(conn, h3_on_close);
    QuicConnectionSetKeepalive(conn, 1);
    h3_add_conn(srv, conn);
}

/* ============================================
 * 公开 API
 * ============================================ */

http3_server *http3_server_create(uv_loop_t *loop,
                                   const char *cert_file,
                                   const char *key_file,
                                   http3_on_request on_req) {
    http3_server *srv = (http3_server*)calloc(1, sizeof(*srv));
    if (!srv) return NULL;

    srv->loop_     = loop;
    srv->on_req_   = on_req;

    srv->listener_ = QuicListenerCreate(loop, cert_file, key_file);
    if (!srv->listener_) { free(srv); return NULL; }

    QuicListenerSetOnConnection(srv->listener_, on_connection);
    QuicListenerSetAppData(srv->listener_, srv);
    return srv;
}

void http3_server_destroy(http3_server *srv) {
    if (!srv) return;

    /* 发送 GOAWAY 帧通知对端优雅关闭 */
    for (int i = 0; i < srv->conn_cnt_; i++) {
        H3Conn *hc = &srv->conns_[i];
        if (!hc->goaway_sent && hc->ctrl_stream_id != 0) {
            uint8_t frm[32];
            int flen = h3_frame_write_goaway(frm, sizeof(frm),
                                              hc->ctrl_stream_id - 4);
            if (flen > 0) {
                QuicConnectionStreamSend(hc->conn, hc->ctrl_stream_id,
                                          frm, (size_t)flen, 1);
                LOG_DEBUG("[h3-server] sent GOAWAY on stream %llu",
                         (unsigned long long)hc->ctrl_stream_id);
            }
            hc->goaway_sent = 1;
        }
    }
    QuicListenerDestruct(srv->listener_);
    free(srv);
}

int http3_server_listen(http3_server *srv, const char *ip, int port) {
    return QuicListenerListen(srv->listener_, ip, port);
}

void  http3_server_set_user_data(http3_server *srv, void *data) { srv->user_data_ = data; }
void* http3_server_get_user_data(http3_server *srv) { return srv->user_data_; }

void http3_server_set_on_datagram(http3_server *srv,
                                   http3_server_on_dgram_fn cb) {
    srv->on_dgram_ = cb;
}

int http3_send_response(QuicConnection *conn, uint64_t stream_id,
                         int status, const char *status_text,
                         const char *content_type,
                         const uint8_t *body, size_t body_len) {
    (void)status_text;
    /* Minimal QPACK HEADERS payload:
     *   required_insert_count(0) + delta_base(0) +
     *   encoded_field_lines (HPACK-style literal, no Huffman)
     *
     * :status 200 → 0x00 | 0x08 (indexed name) + value(len+text)
     * QPACK static table idx 24 = ":status" = 0x00 | 0x17 = 0x17
     * Actually: 0x00 = required insert count 0
     *           0x00 = delta base 0
     *           each line: prefix(1) + name_len + name + value_len + value
     *           prefix = 0x20 | 0x00 = 0x20 (literal, no index, no huffman)
     */
    uint8_t hdr_frame[4096];
    size_t hpos = 0;

    /* QPACK prefix */
    hdr_frame[hpos++] = 0x00; /* required_insert_count = 0 */
    hdr_frame[hpos++] = 0x00; /* delta_base = 0 */

    /* :status — use QPACK static table ref (index 24=:status).
     * QPACK §4.3.2 Literal with Name Ref: 01 | N | T | NameIndex(4+)
     * N=0, T=1(static), NameIndex=24 with 4-bit prefix
     * 4-bit max=15, 24>15 → prefix=15 | continuation=24-15=9 */
    char sval[8];
    int svlen = snprintf(sval, sizeof(sval), "%d", status);

    hdr_frame[hpos++] = 0x5F;  /* 01011111 → 01(NR) | 0(N) | 1(T) | 1111(prefix=15) */
    hdr_frame[hpos++] = 0x09;  /* continuation: 24 - 15 = 9 */
    hpos += quic_varint_encode(hdr_frame + hpos,
                                sizeof(hdr_frame) - hpos,
                                (uint64_t)svlen); /* value length, 7-bit prefix */
    memcpy(hdr_frame + hpos, sval, (size_t)svlen); hpos += (size_t)svlen;

    /* HEADERS frame wrapper */
    uint8_t frame[8192];
    int hlen = h3_frame_write_headers(frame, sizeof(frame),
                                       hdr_frame, hpos);
    if (hlen < 0) return -1;

    int ret = QuicConnectionStreamSend(conn, stream_id, frame, (size_t)hlen, 0);
    if (ret < 0) return ret;

    if (body && body_len > 0) {
        uint8_t dframe[65536 + 8];
        int dlen = h3_frame_write_data(dframe, sizeof(dframe), body, body_len);
        if (dlen < 0) return -1;
        ret = QuicConnectionStreamSend(conn, stream_id, dframe, (size_t)dlen, 1);
    } else {
        QuicConnectionStreamCloseSend(conn, stream_id);
    }
    return ret;
}
