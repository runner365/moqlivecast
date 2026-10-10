#include "quic_client_api.h"
#include "quic_connection.h"
#include "quic_stream.h"
#include "logger.h"
#include <stdlib.h>
#include <string.h>

/* ============================================
 * 内部
 * ============================================ */

#define QC_STATE_IDLE      0
#define QC_STATE_CONNECTING 1
#define QC_STATE_CONNECTED  2
#define QC_STATE_CLOSING    3

enum { QC_TASK_CONNECT, QC_TASK_CLOSE, QC_TASK_OPEN_STREAM,
       QC_TASK_STREAM_WRITE, QC_TASK_STREAM_CLOSE, QC_TASK_STREAM_RESET };

typedef struct qc_task {
    struct qc_task *next;
    int             type;
    /* CONNECT */   char *host; int port;
    /* OPEN_STREAM */ int uni;
    /* STREAM */    quic_stream_t *stream;
    /* WRITE */     uint8_t *data; size_t len; int fin;
                    quic_client_write_cb write_cb; void *write_user;
                    uint64_t timeout_ms;
} qc_task_t;

struct quic_stream {
    quic_client_t *cli;
    uint64_t       stream_id;
    quic_stream_t *next;
};

struct quic_client {
    uv_loop_t              *loop;
    uv_async_t              async;
    quic_client_callbacks_t cb;
    int                     state;
    int                     closing;
    QuicConnection         *qc;
    quic_stream_t          *streams;
    uv_mutex_t              task_mutex;
    qc_task_t              *task_head;
    qc_task_t              *task_tail;

    /* 本客户端要 offer 的 ALPN（NULL = 用 tls_ctx_set_alpn 的写死默认）。
     * 存**副本**而不是借指针：quic_client_set_alpn 可能从任意线程调用，而真正
     * 生效是在 loop 线程执行 CONNECT 任务时 —— 中间调用方很可能已经释放了
     * 自己那份字符串。 */
    char                  **alpn;
    int                     alpn_count;
};

/* ── task queue (mutex) ────────────────────────── */

static void task_push(quic_client_t *c, qc_task_t *t) {
    uv_mutex_lock(&c->task_mutex);
    t->next = NULL;
    if (!c->task_head) c->task_head = c->task_tail = t;
    else c->task_tail->next = t, c->task_tail = t;
    uv_mutex_unlock(&c->task_mutex);
}

static qc_task_t *task_pop(quic_client_t *c) {
    uv_mutex_lock(&c->task_mutex);
    qc_task_t *t = c->task_head;
    if (t) c->task_head = t->next;
    if (!c->task_head) c->task_tail = NULL;
    uv_mutex_unlock(&c->task_mutex);
    return t;
}

/* ── forward ───────────────────────────────────── */

static void async_cb(uv_async_t *h);
static void on_quic_connected(QuicConnection *qc);
static void on_quic_close(QuicConnection *qc, uint64_t err, const char *reason);
static void on_quic_stream_data(QuicConnection *qc, uint64_t sid,
                                 const uint8_t *data, size_t len, int fin);
static void on_quic_stream_reset(QuicConnection *qc, uint64_t sid,
                                  uint64_t error_code);
static void do_open_stream(quic_client_t *c, int uni);
static void do_stream_write(quic_client_t *c, qc_task_t *t);
static quic_stream_t *find_stream(quic_client_t *c, uint64_t sid);

/* ── QUIC write callback → user callback ───────── */

static void qc_write_done(struct QuicStream *qs, int ret,
                           uint64_t len, void *user) {
    (void)qs; (void)len;
    qc_task_t *t = (qc_task_t*)user;
    if (t->write_cb) t->write_cb(t->stream, ret, t->write_user);
    free(t->data);
    free(t);
}

/* ============================================
 * 公开 API
 * ============================================ */

#define QC_DEFAULT_WRITE_TIMEOUT_MS 30000

quic_client_t *quic_client_new(uv_loop_t *loop, quic_client_callbacks_t *cb) {
    quic_client_t *c = (quic_client_t*)calloc(1, sizeof(*c));
    if (!c) return NULL;
    c->loop = loop;
    if (cb) c->cb = *cb; else memset(&c->cb, 0, sizeof(c->cb));
    c->state = QC_STATE_IDLE;
    uv_mutex_init(&c->task_mutex);
    uv_async_init(loop, &c->async, async_cb);
    c->async.data = c;
    return c;
}

void quic_client_connect(quic_client_t *c, const char *ip, int port) {
    if (!c) return;
    qc_task_t *t = (qc_task_t*)calloc(1, sizeof(*t));
    t->type = QC_TASK_CONNECT;
    t->host = strdup(ip);
    t->port = port;
    task_push(c, t);
    uv_async_send(&c->async);
}

void quic_client_set_alpn(quic_client_t *c, const char *const *protos, int count) {
    if (!c) return;

    /* 换掉旧表（可能是第二次调用）。整表替换，不做增量。 */
    if (c->alpn) {
        for (int i = 0; i < c->alpn_count; i++) free(c->alpn[i]);
        free(c->alpn);
        c->alpn = NULL;
        c->alpn_count = 0;
    }
    if (!protos || count <= 0) return;

    char **copy = (char**)calloc((size_t)count, sizeof(char*));
    if (!copy) {
        LOG_WARN("[quic-client] set_alpn: OOM, keeping the default offer");
        return;
    }
    int n = 0;
    for (int i = 0; i < count; i++) {
        if (!protos[i] || !protos[i][0]) continue;
        copy[n] = strdup(protos[i]);
        if (!copy[n]) {
            for (int j = 0; j < n; j++) free(copy[j]);
            free(copy);
            LOG_WARN("[quic-client] set_alpn: OOM, keeping the default offer");
            return;
        }
        n++;
    }
    if (n == 0) { free(copy); return; }
    c->alpn = copy;
    c->alpn_count = n;
}

void quic_client_close(quic_client_t *c) {
    if (!c) return;
    qc_task_t *t = (qc_task_t*)calloc(1, sizeof(*t));
    t->type = QC_TASK_CLOSE;
    task_push(c, t);
    uv_async_send(&c->async);
}

void quic_client_open_stream(quic_client_t *c) {
    if (!c) return;
    qc_task_t *t = (qc_task_t*)calloc(1, sizeof(*t));
    t->type = QC_TASK_OPEN_STREAM;
    t->uni  = 0;
    task_push(c, t);
    uv_async_send(&c->async);
}

void quic_client_open_uni_stream(quic_client_t *c) {
    if (!c) return;
    qc_task_t *t = (qc_task_t*)calloc(1, sizeof(*t));
    t->type = QC_TASK_OPEN_STREAM;
    t->uni  = 1;
    task_push(c, t);
    uv_async_send(&c->async);
}

void quic_client_stream_write(quic_stream_t *s,
                               const uint8_t *data, size_t len, int fin,
                               quic_client_write_cb cb, void *user,
                               uint64_t timeout_ms) {
    if (!s || !s->cli) return;
    qc_task_t *t = (qc_task_t*)calloc(1, sizeof(*t));
    t->type       = QC_TASK_STREAM_WRITE;
    t->stream     = s;
    t->data       = (uint8_t*)malloc(len);
    memcpy(t->data, data, len);
    t->len        = len;
    t->fin        = fin;
    t->write_cb   = cb;
    t->write_user = user;
    t->timeout_ms = timeout_ms ? timeout_ms : QC_DEFAULT_WRITE_TIMEOUT_MS;
    task_push(s->cli, t);
    uv_async_send(&s->cli->async);
}

void quic_client_stream_close(quic_stream_t *s) {
    if (!s || !s->cli) return;
    qc_task_t *t = (qc_task_t*)calloc(1, sizeof(*t));
    t->type   = QC_TASK_STREAM_CLOSE;
    t->stream = s;
    task_push(s->cli, t);
    uv_async_send(&s->cli->async);
}

void quic_client_stream_reset(quic_stream_t *s, uint64_t error_code) {
    if (!s || !s->cli) return;
    qc_task_t *t = (qc_task_t*)calloc(1, sizeof(*t));
    t->type   = QC_TASK_STREAM_RESET;
    t->stream = s;
    t->port   = (int)error_code;   /* 复用 CONNECT 的 port 槽存错误码 */
    task_push(s->cli, t);
    uv_async_send(&s->cli->async);
}

QuicConnection *quic_client_conn(quic_client_t *c) {
    return c ? c->qc : NULL;
}

int quic_client_stream_is_uni(const quic_stream_t *s) {
    return s ? ((s->stream_id & 0x2) ? 1 : 0) : 0;
}

/* ============================================
 * loop 线程 — async_cb
 * ============================================ */

static void async_cb(uv_async_t *h) {
    quic_client_t *c = (quic_client_t*)h->data;
    qc_task_t *t;
    while ((t = task_pop(c))) {
        switch (t->type) {
        case QC_TASK_CONNECT:
            if (c->state == QC_STATE_IDLE) {
                c->state = QC_STATE_CONNECTING;
                c->qc = QuicConnectionCreate(c->loop);
                QuicConnectionSetIdleTimeout(c->qc, 300000);
                QuicConnectionSetAppData(c->qc, c);
                QuicConnectionSetOnConnected(c->qc, on_quic_connected);
                QuicConnectionSetOnStreamData(c->qc, on_quic_stream_data);
                QuicConnectionSetOnStreamReset(c->qc, on_quic_stream_reset);
                QuicConnectionSetOnClose(c->qc, on_quic_close);
                /* ALPN 必须在 Connect 之前设 —— 它设的是已建好的 SSL 对象，
                 * 握手一发起 offer 就发出去了。不设时保持 tls_ctx_set_alpn
                 * 的写死默认（h3 + quic-echo），老调用方行为不变。 */
                if (c->alpn && c->alpn_count > 0) {
                    QuicConnectionSetAlpnList(c->qc,
                                              (const char *const *)c->alpn,
                                              c->alpn_count);
                }
                QuicConnectionConnect(c->qc, t->host, t->port);
            }
            break;
        case QC_TASK_OPEN_STREAM:
            if (c->state == QC_STATE_CONNECTED && !c->closing)
                do_open_stream(c, t->uni);
            break;
        case QC_TASK_STREAM_WRITE:
            do_stream_write(c, t);
            continue; /* task owned by QUIC → freed in qc_write_done */
        case QC_TASK_STREAM_CLOSE:
            if (t->stream && t->stream->stream_id)
                QuicConnectionStreamCloseSend(c->qc, t->stream->stream_id);
            break;
        case QC_TASK_STREAM_RESET:
            if (t->stream && t->stream->stream_id)
                QuicConnectionStreamReset(c->qc, t->stream->stream_id,
                                          (uint64_t)t->port);
            break;
        case QC_TASK_CLOSE:
            c->closing = 1; c->state = QC_STATE_CLOSING;
            /* ALPN 表用完即弃 —— 连接关了它不再有意义，留着只是内存。
             * （本 API 不负责释放 quic_client_t 自身，与既有行为一致。） */
            if (c->alpn) {
                for (int i = 0; i < c->alpn_count; i++) free(c->alpn[i]);
                free(c->alpn);
                c->alpn = NULL;
                c->alpn_count = 0;
            }
            if (c->qc) QuicConnectionClose(c->qc, 0, "client close");
            break;
        }
        free(t->host);
        free(t->data);
        free(t);
    }
}

/* ============================================
 * stream 管理
 * ============================================ */

static quic_stream_t *find_stream(quic_client_t *c, uint64_t sid) {
    for (quic_stream_t *s = c->streams; s; s = s->next)
        if (s->stream_id == sid) return s;
    return NULL;
}

static void do_open_stream(quic_client_t *c, int uni) {
    uint64_t sid = uni ? QuicConnectionStreamOpenUni(c->qc)
                       : QuicConnectionStreamOpen(c->qc);
    if (sid == UINT64_MAX) {
        LOG_WARN("[qc-api] open %s stream failed", uni ? "uni" : "bidi");
        return;
    }
    quic_stream_t *s = (quic_stream_t*)calloc(1, sizeof(*s));
    s->cli = c; s->stream_id = sid;
    s->next = c->streams; c->streams = s;
    if (uni) {
        if (c->cb.on_stream_open_uni)
            c->cb.on_stream_open_uni(c, s, c->cb.user_data);
    } else if (c->cb.on_stream_open) {
        c->cb.on_stream_open(c, s, c->cb.user_data);
    }
    LOG_DEBUG("[qc-api] opened %s stream %llu", uni ? "uni" : "bidi",
              (unsigned long long)sid);
}

static void do_stream_write(quic_client_t *c, qc_task_t *t) {
    if (!t->stream || !c->qc) return;
    QuicConnectionStreamSendEx(c->qc, t->stream->stream_id,
                                t->data, t->len, t->fin,
                                qc_write_done, t, t->timeout_ms);
}

/* ============================================
 * QUIC 回调
 * ============================================ */

static void on_quic_connected(QuicConnection *qc) {
    quic_client_t *c = (quic_client_t*)QuicConnectionGetAppData(qc);
    if (!c) return;
    c->state = QC_STATE_CONNECTED;
    if (c->cb.on_connect) c->cb.on_connect(c, 0, c->cb.user_data);
}

static void on_quic_close(QuicConnection *qc, uint64_t err, const char *reason) {
    (void)reason;
    quic_client_t *c = (quic_client_t*)QuicConnectionGetAppData(qc);
    if (!c) return;
    if (c->cb.on_close) c->cb.on_close(c, (int)err, c->cb.user_data);
}

static void on_quic_stream_data(QuicConnection *qc, uint64_t sid,
                                 const uint8_t *data, size_t len, int fin) {
    quic_client_t *c = (quic_client_t*)QuicConnectionGetAppData(qc);
    if (!c) return;
    quic_stream_t *s = find_stream(c, sid);
    if (!s) {
        s = (quic_stream_t*)calloc(1, sizeof(*s));
        s->cli = c; s->stream_id = sid;
        s->next = c->streams; c->streams = s;
    }
    if (c->cb.on_stream_data)
        c->cb.on_stream_data(c, s, data, len, c->cb.user_data);
    (void)fin;
}

static void on_quic_stream_reset(QuicConnection *qc, uint64_t sid,
                                  uint64_t error_code) {
    quic_client_t *c = (quic_client_t*)QuicConnectionGetAppData(qc);
    if (!c) return;
    /* s 可能是 NULL：对端可以 reset 一条我们只发过、没收过数据的流，
     * 那时本地还没建记录。照样把通知发出去 —— 上层判的是「被拒」这个事实，
     * 不依赖它有没有那条流的本地记录。 */
    quic_stream_t *s = find_stream(c, sid);
    if (c->cb.on_stream_reset)
        c->cb.on_stream_reset(c, s, error_code, c->cb.user_data);
}
