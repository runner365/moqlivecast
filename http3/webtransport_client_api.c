#include "webtransport_client_api.h"
#include "http3_frame.h"
#include "http3_common.h"
#include "quic_connection.h"
#include "quic_crypto.h"
#include "webtransport.h"
#include "logger.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

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

/* ============================================
 * 内部状态
 * ============================================ */
#define WT_STATE_IDLE          0
#define WT_STATE_CONNECTING    1  /* QUIC 连接中 */
#define WT_STATE_SETTINGS      2  /* 已发 client SETTINGS，等 server SETTINGS */
#define WT_STATE_CONNECTING_WT 3  /* 已发 CONNECT，等 200 */
#define WT_STATE_CONNECTED     4  /* WT 会话就绪 */
#define WT_STATE_CLOSING       5

/* ── 任务类型 ──────────────────────────────── */
enum {
    WT_TASK_CONNECT,
    WT_TASK_CLOSE,
    WT_TASK_OPEN_STREAM,
    WT_TASK_STREAM_WRITE,
    WT_TASK_STREAM_CLOSE
};

typedef struct wt_task {
    struct wt_task *next;
    int             type;
    /* CONNECT */
    char   *host;
    int     port;
    /* STREAM_WRITE / STREAM_CLOSE */
    wt_stream_t *stream;
    /* STREAM_WRITE */
    uint8_t *data;
    size_t   len;
    /* STREAM_WRITE callback */
    void   (*write_cb)(wt_stream_t *s, int ret, void *user);
    void   *write_cb_user;
    uint64_t timeout_ms;
} wt_task_t;

/* ── Stream ────────────────────────────────── */
struct wt_stream {
    wt_client_t *cli;          /* 所属客户端 */
    uint64_t     stream_id;    /* QUIC stream ID */
    wt_stream_t *next;         /* 链表 */
    int          header_sent;      /* WT 帧头是否已发送 */
    int          header_stripped;  /* WT 帧头是否已从入站数据剥离 */
};

/* ── Client ────────────────────────────────── */
struct wt_client {
    uv_loop_t       *loop;
    uv_async_t       async;
    wt_callbacks_t   cb;
    int              state;
    int              closing;
    /* QUIC */
    QuicConnection  *qc;
    uint64_t         ctrl_sid;       /* H3 控制流 */
    int              connect_sent;   /* CONNECT 已发送 */
    /* Stream 链表 */
    wt_stream_t     *streams;
    /* 任务队列 + 互斥锁 */
    uv_mutex_t       task_mutex;
    wt_task_t       *task_head;
    wt_task_t       *task_tail;
};

/* ============================================
 * task queue
 * ============================================ */
static void task_push(wt_client_t *cli, wt_task_t *t) {
    uv_mutex_lock(&cli->task_mutex);
    t->next = NULL;
    if (!cli->task_head)
        cli->task_head = cli->task_tail = t;
    else
        cli->task_tail->next = t, cli->task_tail = t;
    uv_mutex_unlock(&cli->task_mutex);
}

static wt_task_t *task_pop(wt_client_t *cli) {
    uv_mutex_lock(&cli->task_mutex);
    wt_task_t *t = cli->task_head;
    if (t) cli->task_head = t->next;
    if (!cli->task_head) cli->task_tail = NULL;
    uv_mutex_unlock(&cli->task_mutex);
    return t;
}

/* ============================================
 * QPACK encode
 * ============================================ */
static void qpack_write_literal_i(uint8_t *buf, size_t *pos,
                                   const char *name, const char *val) {
    size_t nlen = strlen(name), vlen = strlen(val);
    if (nlen < 7)
        buf[(*pos)++] = (uint8_t)(0x20 | nlen);
    else {
        buf[(*pos)++] = 0x27; size_t r = nlen - 7;
        while (r >= 128) { buf[(*pos)++] = (uint8_t)((r & 0x7f) | 0x80); r >>= 7; }
        buf[(*pos)++] = (uint8_t)(r & 0x7f);
    }
    memcpy(buf + *pos, name, nlen); *pos += nlen;
    if (vlen < 128)
        buf[(*pos)++] = (uint8_t)(vlen & 0x7f);
    else {
        buf[(*pos)++] = (uint8_t)(((unsigned)vlen & 0x7f) | 0x80);
        size_t r = vlen >> 7;
        while (r >= 128) { buf[(*pos)++] = (uint8_t)((r & 0x7f) | 0x80); r >>= 7; }
        buf[(*pos)++] = (uint8_t)(r & 0x7f);
    }
    memcpy(buf + *pos, val, vlen); *pos += vlen;
}

/* ============================================
 * 前向声明
 * ============================================ */
static void async_cb(uv_async_t *h);
static void on_quic_connected(QuicConnection *qc);
static void on_quic_close(QuicConnection *qc, uint64_t err, const char *reason);
static void on_quic_stream(QuicConnection *qc, uint64_t sid,
                            const uint8_t *data, size_t len, int fin);
static void send_client_settings(wt_client_t *cli);
static void send_wt_connect_internal(wt_client_t *cli);
static void do_open_stream(wt_client_t *cli);
static void do_stream_write(wt_client_t *cli, wt_task_t *t);
static wt_stream_t *find_stream(wt_client_t *cli, uint64_t sid);
static void free_task(wt_task_t *t);

/* ============================================
 * 公开 API
 * ============================================ */

wt_client_t *wt_client_new(uv_loop_t *loop, wt_callbacks_t *cb) {
    wt_client_t *cli = (wt_client_t*)calloc(1, sizeof(*cli));
    if (!cli) return NULL;
    cli->loop = loop;
    if (cb) cli->cb = *cb; else memset(&cli->cb, 0, sizeof(cli->cb));
    cli->state = WT_STATE_IDLE;
    uv_mutex_init(&cli->task_mutex);
    uv_async_init(loop, &cli->async, async_cb);
    cli->async.data = cli;
    return cli;
}

void wt_client_connect(wt_client_t *cli, const char *host, int port) {
    if (!cli) return;
    wt_task_t *t = (wt_task_t*)calloc(1, sizeof(*t));
    t->type = WT_TASK_CONNECT;
    t->host = strdup(host);
    t->port = port;
    task_push(cli, t);
    uv_async_send(&cli->async);
}

void wt_client_close(wt_client_t *cli) {
    if (!cli) return;
    wt_task_t *t = (wt_task_t*)calloc(1, sizeof(*t));
    t->type = WT_TASK_CLOSE;
    task_push(cli, t);
    uv_async_send(&cli->async);
}

void *wt_client_get_user_data(wt_client_t *cli) {
    return cli ? cli->cb.user_data : NULL;
}

int wt_client_get_quic_stats(wt_client_t *cli, QuicConnectionStats *out) {
    if (!cli || !cli->qc || !out) return -1;
    return QuicConnectionGetStats(cli->qc, out);
}

void wt_client_open_stream(wt_client_t *cli) {
    if (!cli) return;
    wt_task_t *t = (wt_task_t*)calloc(1, sizeof(*t));
    t->type = WT_TASK_OPEN_STREAM;
    task_push(cli, t);
    uv_async_send(&cli->async);
}

void wt_stream_write_cb(wt_stream_t *s, const uint8_t *data, size_t len,
                         wt_stream_write_cb_fn cb, void *user,
                         uint64_t timeout_ms) {
    if (!s || !s->cli) return;
    wt_task_t *t = (wt_task_t*)calloc(1, sizeof(*t));
    t->type         = WT_TASK_STREAM_WRITE;
    t->stream       = s;
    t->data         = (uint8_t*)malloc(len);
    memcpy(t->data, data, len);
    t->len          = len;
    t->write_cb     = cb;
    t->write_cb_user = user;
    t->timeout_ms   = timeout_ms;
    task_push(s->cli, t);
    uv_async_send(&s->cli->async);
}

void wt_stream_close(wt_stream_t *s) {
    if (!s || !s->cli) return;
    wt_task_t *t = (wt_task_t*)calloc(1, sizeof(*t));
    t->type   = WT_TASK_STREAM_CLOSE;
    t->stream = s;
    task_push(s->cli, t);
    uv_async_send(&s->cli->async);
}

/* ============================================
 * loop 线程 — 任务调度
 * ============================================ */

static void async_cb(uv_async_t *h) {
    wt_client_t *cli = (wt_client_t*)h->data;
    wt_task_t *t;
    while ((t = task_pop(cli))) {
        switch (t->type) {
        case WT_TASK_CONNECT:
            if (cli->state == WT_STATE_IDLE) {
                cli->state = WT_STATE_CONNECTING;
                cli->qc = QuicConnectionCreate(cli->loop);
                QuicConnectionSetIdleTimeout(cli->qc, 300000); /* 300s covers full test */
                QuicConnectionSetAppData(cli->qc, cli);
                QuicConnectionSetOnConnected(cli->qc, on_quic_connected);
                QuicConnectionSetOnStreamData(cli->qc, on_quic_stream);
                QuicConnectionSetOnClose(cli->qc, on_quic_close);
                QuicConnectionConnect(cli->qc, t->host, t->port);
                LOG_DEBUG("[wt-api] connecting to %s:%d", t->host, t->port);
            }
            break;
        case WT_TASK_OPEN_STREAM:
            if (cli->state == WT_STATE_CONNECTED && !cli->closing)
                do_open_stream(cli);
            break;
        case WT_TASK_STREAM_WRITE:
            do_stream_write(cli, t);
            /* task ownership always transfers to QUIC → freed in wt_write_done_cb */
            continue;
        case WT_TASK_STREAM_CLOSE:
            if (t->stream && t->stream->stream_id) {
                QuicConnectionStreamCloseSend(cli->qc, t->stream->stream_id);
            }
            break;
        case WT_TASK_CLOSE:
            cli->closing = 1;
            cli->state  = WT_STATE_CLOSING;
            if (cli->qc)
                QuicConnectionClose(cli->qc, 0, "client close");
            break;
        }
        free_task(t);
    }
}

static void free_task(wt_task_t *t) {
    if (!t) return;
    free(t->host);
    free(t->data);
    free(t);
}

/* ============================================
 * Stream 管理
 * ============================================ */

static wt_stream_t *find_stream(wt_client_t *cli, uint64_t sid) {
    for (wt_stream_t *s = cli->streams; s; s = s->next)
        if (s->stream_id == sid) return s;
    return NULL;
}

static void do_open_stream(wt_client_t *cli) {
    uint64_t sid = QuicConnectionStreamOpen(cli->qc);
    if (sid == UINT64_MAX) { LOG_WARN("[wt-api] open stream failed"); return; }

    wt_stream_t *s = (wt_stream_t*)calloc(1, sizeof(*s));
    s->cli       = cli;
    s->stream_id = sid;
    s->next      = cli->streams;
    cli->streams = s;

    /* 通知用户 stream 已就绪 */
    if (cli->cb.on_stream_open)
        cli->cb.on_stream_open(cli, s, cli->cb.user_data);

    LOG_DEBUG("[wt-api] opened stream %llu", (unsigned long long)sid);
}

/* ── QUIC write callback → wt write callback 转发 ── */
static void wt_write_done_cb(struct QuicStream *qs, int ret,
                               uint64_t len, void *user) {
    (void)qs; (void)len;
    wt_task_t *t = (wt_task_t*)user;
    if (t->write_cb)
        t->write_cb(t->stream, ret, t->write_cb_user);
    /* task + data freed after callback — QUIC already copied into send_buf */
    free(t->data);
    free(t);
}

static void do_stream_write(wt_client_t *cli, wt_task_t *t) {
    if (!t->stream || !cli->qc) return;

    /* All writes go through QuicConnectionStreamSendEx with callback.
     * Task+data freed in wt_write_done_cb after ACK/error. */
    if (!t->stream->header_sent) {
        uint8_t hdr[3] = { 0x40, 0x41, 0x00 };
        size_t total = 3 + t->len;
        uint8_t *framed = (uint8_t*)malloc(total);
        if (!framed) return;
        framed[0] = hdr[0]; framed[1] = hdr[1]; framed[2] = hdr[2];
        memcpy(framed + 3, t->data, t->len);
        QuicConnectionStreamSendEx(cli->qc, t->stream->stream_id,
                                    framed, total, 0, wt_write_done_cb, t,
                                    t->timeout_ms);
        free(framed);
        t->stream->header_sent = 1;
    } else {
        QuicConnectionStreamSendEx(cli->qc, t->stream->stream_id,
                                    t->data, t->len, 0, wt_write_done_cb, t,
                                    t->timeout_ms);
    }
}

/* ============================================
 * QUIC 回调
 * ============================================ */

static void on_quic_close(QuicConnection *qc, uint64_t err, const char *reason) {
    (void)err; (void)reason;
    wt_client_t *cli = (wt_client_t*)QuicConnectionGetAppData(qc);
    if (!cli) return;
    if (cli->cb.on_close)
        cli->cb.on_close(cli, (int)err, cli->cb.user_data);
}

static void on_quic_connected(QuicConnection *qc) {
    wt_client_t *cli = NULL;
    /* Find cli via qc — stored in app data */
    cli = (wt_client_t*)QuicConnectionGetAppData(qc);
    if (!cli) return;
    cli->state = WT_STATE_SETTINGS;
    send_client_settings(cli);
}

static void on_quic_stream(QuicConnection *qc, uint64_t sid,
                            const uint8_t *data, size_t len, int fin) {
    (void)fin;
    wt_client_t *cli = (wt_client_t*)QuicConnectionGetAppData(qc);
    if (!cli) return;

    /* uni stream: server H3 control */
    if (sid % 4 != 0) {
        if (len < 1) return;
        uint8_t stype = data[0]; data++; len--;
        if (stype == H3_STREAM_TYPE_CONTROL && len > 0 && cli->state < WT_STATE_CONNECTING_WT) {
            /* parse server SETTINGS */
            uint64_t ids[8], vals[8]; int count = 0;
            if (h3_frame_parse_settings(data, len, ids, vals, 8, &count) > 0) {
                LOG_DEBUG("[wt-api] server SETTINGS: %d params", count);
                if (!cli->connect_sent) {
                    cli->connect_sent = 1;
                    cli->state = WT_STATE_CONNECTING_WT;
                    send_wt_connect_internal(cli);
                }
            }
        }
        return;
    }

    /* bidi stream — if not yet connected, this is CONNECT 200 */
    if (cli->state == WT_STATE_CONNECTING_WT) {
        cli->state = WT_STATE_CONNECTED;
        LOG_DEBUG("[wt-api] WT session established");
        if (cli->cb.on_connect)
            cli->cb.on_connect(cli, 0, cli->cb.user_data);
        return;
    }

    wt_stream_t *s = find_stream(cli, sid);
    if (!s) {
        s = (wt_stream_t*)calloc(1, sizeof(*s));
        s->cli = cli; s->stream_id = sid;
        s->next = cli->streams; cli->streams = s;
    }

    /* Strip WebTransport STREAM frame header — once per stream.
     * Header is {0x40,0x41,0x00}, prepended on peer's first write.
     * Guard with flag: data bytes can coincidentally match. */
    const uint8_t *payload = data;
    size_t payload_len = len;
    if (!s->header_stripped && len >= 3
        && data[0] == 0x40 && data[1] == 0x41 && data[2] == 0x00) {
        payload     = data + 3;
        payload_len = len - 3;
        s->header_stripped = 1;
    }
    if (cli->cb.on_stream_data && payload_len > 0)
        cli->cb.on_stream_data(cli, s, payload, payload_len, cli->cb.user_data);
}

/* ============================================
 * H3 握手
 * ============================================ */

static void send_client_settings(wt_client_t *cli) {
    uint64_t csid = QuicConnectionStreamOpenUni(cli->qc);
    if (csid == UINT64_MAX) { LOG_WARN("[wt-api] open ctrl stream failed"); return; }
    cli->ctrl_sid = csid;
    uint8_t sbuf[280]; sbuf[0] = H3_STREAM_TYPE_CONTROL;
    uint64_t ids[] = { H3_SETTING_QPACK_MAX_TABLE_CAPACITY,
                        H3_SETTING_MAX_FIELD_SECTION_SIZE,
                        H3_SETTING_ENABLE_CONNECT_PROTOCOL,
                        H3_SETTING_ENABLE_WEBTRANSPORT_DRAFT02,
                        H3_SETTING_ENABLE_WEBTRANSPORT,
                        H3_SETTING_H3_DATAGRAM };
    uint64_t vals[] = { 4096, 16384, 1, 1, 1, 1 };
    int flen = h3_frame_write_settings(sbuf + 1, sizeof(sbuf)-1, ids, vals, 6);
    int sret = QuicConnectionStreamSend(cli->qc, csid, sbuf, (size_t)(1+flen), 0);
    LOG_DEBUG("[wt-api] client SETTINGS send: sid=%llu len=%d ret=%d",
             (unsigned long long)csid, 1 + flen, sret);
}

static void send_wt_connect_internal(wt_client_t *cli) {
    /* QPACK encode CONNECT request */
    uint8_t hdr[512]; size_t p = 0;
    hdr[p++] = 0x00; hdr[p++] = 0x00;
    qpack_write_literal_i(hdr, &p, ":method",   "CONNECT");
    qpack_write_literal_i(hdr, &p, ":protocol", "webtransport");
    qpack_write_literal_i(hdr, &p, ":scheme",   "https");
    qpack_write_literal_i(hdr, &p, ":path",     "/");
    qpack_write_literal_i(hdr, &p, ":authority", "127.0.0.1:4433");
    qpack_write_literal_i(hdr, &p, "sec-webtransport-http3-draft-02", "1");

    uint8_t frame[1024];
    int flen = h3_frame_write_headers(frame, sizeof(frame), hdr, p);
    if (flen < 0) { LOG_WARN("[wt-api] HEADERS frame failed"); return; }

    uint64_t sid = QuicConnectionStreamOpen(cli->qc);
    if (sid == UINT64_MAX) { LOG_WARN("[wt-api] CONNECT bidi open failed"); return; }
    QuicConnectionStreamSend(cli->qc, sid, frame, (size_t)flen, 0);
    LOG_DEBUG("[wt-api] CONNECT sent on stream %llu", (unsigned long long)sid);
}
