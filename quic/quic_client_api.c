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
       QC_TASK_STREAM_WRITE, QC_TASK_STREAM_CLOSE };

typedef struct qc_task {
    struct qc_task *next;
    int             type;
    /* CONNECT */   char *host; int port;
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
static void do_open_stream(quic_client_t *c);
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
                QuicConnectionSetOnClose(c->qc, on_quic_close);
                QuicConnectionConnect(c->qc, t->host, t->port);
            }
            break;
        case QC_TASK_OPEN_STREAM:
            if (c->state == QC_STATE_CONNECTED && !c->closing)
                do_open_stream(c);
            break;
        case QC_TASK_STREAM_WRITE:
            do_stream_write(c, t);
            continue; /* task owned by QUIC → freed in qc_write_done */
        case QC_TASK_STREAM_CLOSE:
            if (t->stream && t->stream->stream_id)
                QuicConnectionStreamCloseSend(c->qc, t->stream->stream_id);
            break;
        case QC_TASK_CLOSE:
            c->closing = 1; c->state = QC_STATE_CLOSING;
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

static void do_open_stream(quic_client_t *c) {
    uint64_t sid = QuicConnectionStreamOpen(c->qc);
    if (sid == UINT64_MAX) { LOG_WARN("[qc-api] open stream failed"); return; }
    quic_stream_t *s = (quic_stream_t*)calloc(1, sizeof(*s));
    s->cli = c; s->stream_id = sid;
    s->next = c->streams; c->streams = s;
    if (c->cb.on_stream_open) c->cb.on_stream_open(c, s, c->cb.user_data);
    LOG_DEBUG("[qc-api] opened stream %llu", (unsigned long long)sid);
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
