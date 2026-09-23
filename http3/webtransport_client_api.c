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
    WT_TASK_OPEN_UNI_STREAM,
    WT_TASK_STREAM_WRITE,
    WT_TASK_STREAM_CLOSE
};

typedef struct wt_task {
    struct wt_task *next;
    int             type;
    /* CONNECT */
    char   *host;
    int     port;
    char   *path;
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
    int          is_uni;           /* 1 = 单向流（只能写，不能读） */
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
    /* CONNECT 请求的 :path / :authority（默认 "/" 与 "127.0.0.1:4433"）。
     * 长度上限与 send_wt_connect_internal 的 hdr[512] 配套，勿随意放大。 */
    char             path[192];
    char             authority[96];
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
static void do_open_stream(wt_client_t *cli, int is_uni);
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

void wt_client_connect_path(wt_client_t *cli, const char *host, int port,
                            const char *path) {
    if (!cli || !host) return;
    wt_task_t *t = (wt_task_t*)calloc(1, sizeof(*t));
    t->type = WT_TASK_CONNECT;
    t->host = strdup(host);
    t->port = port;
    t->path = strdup(path && path[0] ? path : "/");
    task_push(cli, t);
    uv_async_send(&cli->async);
}

void wt_client_connect(wt_client_t *cli, const char *host, int port) {
    wt_client_connect_path(cli, host, port, "/");
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

void wt_client_open_uni_stream(wt_client_t *cli) {
    if (!cli) return;
    wt_task_t *t = (wt_task_t*)calloc(1, sizeof(*t));
    t->type = WT_TASK_OPEN_UNI_STREAM;
    task_push(cli, t);
    uv_async_send(&cli->async);
}

int wt_stream_is_uni(wt_stream_t *s) {
    return s ? s->is_uni : 0;
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
                /* 在 loop 线程上落盘 :path / :authority，供 CONNECT 使用 */
                snprintf(cli->path, sizeof(cli->path), "%s",
                         t->path ? t->path : "/");
                snprintf(cli->authority, sizeof(cli->authority), "%s:%d",
                         t->host, t->port);
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
                do_open_stream(cli, 0);
            break;
        case WT_TASK_OPEN_UNI_STREAM:
            if (cli->state == WT_STATE_CONNECTED && !cli->closing)
                do_open_stream(cli, 1);
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
    free(t->path);
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

static void do_open_stream(wt_client_t *cli, int is_uni) {
    uint64_t sid = is_uni ? QuicConnectionStreamOpenUni(cli->qc)
                          : QuicConnectionStreamOpen(cli->qc);
    if (sid == UINT64_MAX) {
        LOG_WARN("[wt-api] open %s stream failed", is_uni ? "uni" : "bidi");
        return;
    }

    wt_stream_t *s = (wt_stream_t*)calloc(1, sizeof(*s));
    s->cli       = cli;
    s->stream_id = sid;
    s->is_uni    = is_uni;
    s->next      = cli->streams;
    cli->streams = s;

    /* 通知用户 stream 已就绪 */
    if (cli->cb.on_stream_open)
        cli->cb.on_stream_open(cli, s, cli->cb.user_data);

    LOG_DEBUG("[wt-api] opened %s stream %llu", is_uni ? "uni" : "bidi",
              (unsigned long long)sid);
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

    /* 对端发起的单向流不能回写（低 2 位 0x3 = server-initiated uni），
     * 发出去会触发对端 PROTOCOL_VIOLATION。本地发起的 uni（0x2）不受
     * 限制 —— 那条流本来就是给我们写的。 */
    if ((t->stream->stream_id & 0x3) == 0x3) {
        LOG_WARN("[wt-api] refuse write on peer-initiated uni stream %llu "
                 "(%zu bytes dropped)",
                 (unsigned long long)t->stream->stream_id, t->len);
        if (t->write_cb) t->write_cb(t->stream, -1, t->write_cb_user);
        free(t->data);
        free(t);
        return;
    }

    /* All writes go through QuicConnectionStreamSendEx with callback.
     * Task+data freed in wt_write_done_cb after ACK/error.
     *
     * 首包前缀按流类型区分 —— 两者是不同的协议元素，不能混用：
     *   双向流：WT STREAM 帧 {0x40,0x41,0x00}（frame type 0x41 + session 0）
     *   单向流：WT 流类型 0x54（draft-ietf-webtrans-http3），标识「本流是
     *           WebTransport 数据流」。服务端在 http3_server.c 用
     *           quic_varint_decode 读首字节并与 0x54 比对，发错会被
     *           当成未知 uni 流丢弃（实测服务端报 type=0x40 not WT 0x54）。 */
    if (!t->stream->header_sent) {
        /* 首个 varint 是「WT 流类型」，其后紧跟 session_id（varint）。
         *
         * 双向流用 0x41（WT STREAM 帧）；单向流用 0x54
         * （draft-ietf-webtrans-http3 的单向数据流类型）。
         *
         * ⚠️ 0x54 必须按 varint 编码成 2 字节 0x40 0x54，不能写成单字节：
         *    0x54 = 0b01010100，高 2 位是 01，quic_varint_decode 会按
         *    2 字节读，得到 ((0x54 & 0x3f) << 8) | next ≠ 0x54，
         *    服务端于是判定「不是 WT 流」直接丢弃。
         *
         * 另注意服务端有两处检查，0x54 恰好同时满足：
         *    http3_server.c       首 varint == 0x54      → 识别为 WT 单向流
         *    webtransport_server_api.c  首 varint ∈ [0x41,0x5f] → 剥离帧头
         *    0x54 ∈ [0x41,0x5f]，所以结构与双向流一致，只是类型值不同。 */
        size_t hdr_len;
        uint8_t hdr[4];
        const uint64_t stream_type = (t->stream->is_uni &&
                                      (t->stream->stream_id & 0x3) == 0x2)
                                         ? H3_STREAM_TYPE_WEBTRANSPORT  /* 0x54 */
                                         : 0x41;
        if (stream_type < 64) {
            hdr[0] = (uint8_t)stream_type;
            hdr_len = 1;
        } else {
            hdr[0] = (uint8_t)(0x40 | ((stream_type >> 8) & 0x3f));
            hdr[1] = (uint8_t)(stream_type & 0xff);
            hdr_len = 2;
        }
        hdr[hdr_len++] = 0x00;   /* session_id = 0（varint） */
        size_t total = hdr_len + t->len;
        uint8_t *framed = (uint8_t*)malloc(total);
        if (!framed) return;
        memcpy(framed, hdr, hdr_len);
        memcpy(framed + hdr_len, t->data, t->len);
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

    /* ── 服务端发起的单向流 (sid & 0x3) == 0x3 ──
     *
     * 原判据 `sid % 4 != 0` 过宽：它把 server bidi(0x1)、client uni(0x2)
     * 也一并截住。对 H3 控制流（确实是 server uni）碰巧正确，但一旦
     * 服务端在 uni 流上发 WT 数据（MOQ 规范要求对象走单向流），
     * 这些数据会被当成控制流丢弃。
     *
     * 正确做法：只截 server uni，且仅当首字节是 H3 控制流类型时才
     * 按 SETTINGS 解析；其余 server uni 流继续走下面的 WT 数据路径。 */
    if ((sid & 0x3) == 0x3) {
        /* 先按 H3 流类型分流。四种内建类型都必须拦下，只放行 0x54：
         *   0x00 control / 0x01 push / 0x02 QPACK enc / 0x03 QPACK dec
         * 遗漏任何一个都会把 H3 内部字节泄露给应用层 —— 例如服务端开
         * QPACK 流时先写 1 字节类型 0x02，若放行就成了 WT 数据的首字节，
         * 破坏上层（LOC）解析。 */
        if (len >= 1) {
            uint64_t stype = 0;
            size_t stlen = quic_varint_read(data, len, &stype);
            if (stlen > 0 && stype <= H3_STREAM_TYPE_QPACK_DEC) {
                if (stype == H3_STREAM_TYPE_CONTROL) {
                    const uint8_t *sdata = data + stlen;
                    size_t slen = len - stlen;
                    if (slen > 0 && cli->state < WT_STATE_CONNECTING_WT) {
                        uint64_t ids[8], vals[8]; int count = 0;
                        if (h3_frame_parse_settings(sdata, slen, ids, vals, 8,
                                                    &count) > 0) {
                            LOG_DEBUG("[wt-api] server SETTINGS: %d params", count);
                            if (!cli->connect_sent) {
                                cli->connect_sent = 1;
                                cli->state = WT_STATE_CONNECTING_WT;
                                send_wt_connect_internal(cli);
                            }
                        }
                    }
                }
                return;   /* H3 内建流：不产生应用层数据 */
            }
        }
        /* 其余 server uni 流（含 WT 数据流 0x54）→ 落到下方 WT 数据路径 */
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
        /* 对端发起的流：低 2 位 0x2/0x3 即单向。
         * 记录以便上层判断「这条流能否回写」。 */
        s->is_uni = ((sid & 0x3) == 0x2 || (sid & 0x3) == 0x3);
        s->next = cli->streams; cli->streams = s;
    }

    /* Strip WebTransport header — once per stream.
     * 格式为「流类型 varint + session_id varint」，不能用硬编码字节比对：
     *   双向流类型 0x41（1 字节）
     *   单向流类型 0x54（varint 编码成 2 字节 0x40 0x54）
     * 原实现写死 {0x40,0x41,0x00}，遇到服务端单向流的 0x40 0x54 0x00
     * 匹配不上，会把 3 字节头当成载荷交给上层（内容前带乱码）。
     * 改为按 varint 解析：首 token 落在 [0x41,0x5f] 即视为 WT 头。 */
    const uint8_t *payload = data;
    size_t payload_len = len;
    if (!s->header_stripped && len >= 2) {
        uint64_t ftype = 0;
        size_t n = quic_varint_read(data, len, &ftype);
        if (n > 0 && ftype >= 0x41 && ftype <= 0x5f) {
            uint64_t sess_id = 0;
            size_t m = (n < len) ? quic_varint_read(data + n, len - n, &sess_id) : 0;
            if (m > 0) {
                payload     = data + n + m;
                payload_len = len - n - m;
                s->header_stripped = 1;
            }
        }
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
    qpack_write_literal_i(hdr, &p, ":path",     cli->path[0] ? cli->path : "/");
    qpack_write_literal_i(hdr, &p, ":authority",
                          cli->authority[0] ? cli->authority : "127.0.0.1:4433");
    qpack_write_literal_i(hdr, &p, "sec-webtransport-http3-draft-02", "1");

    if (p >= sizeof(hdr)) {
        LOG_WARN("[wt-api] CONNECT header overflow (%zu bytes), path too long", p);
        return;
    }
    uint8_t frame[1024];
    int flen = h3_frame_write_headers(frame, sizeof(frame), hdr, p);
    if (flen < 0) { LOG_WARN("[wt-api] HEADERS frame failed"); return; }

    uint64_t sid = QuicConnectionStreamOpen(cli->qc);
    if (sid == UINT64_MAX) { LOG_WARN("[wt-api] CONNECT bidi open failed"); return; }
    QuicConnectionStreamSend(cli->qc, sid, frame, (size_t)flen, 0);
    LOG_DEBUG("[wt-api] CONNECT sent on stream %llu", (unsigned long long)sid);
}
