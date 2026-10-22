#include "quic_client_api.h"
#include "quic_connection.h"
#include "quic_stream.h"
#include "logger.h"
#include <netdb.h>      /* struct addrinfo / AF_INET / SOCK_DGRAM —— uv_getaddrinfo 用 */
#include <stdio.h>
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
    int                     freeing;   /* 已排入 uv_close，防重复 free */
    QuicConnection         *qc;
    quic_stream_t          *streams;
    uv_mutex_t              task_mutex;
    qc_task_t              *task_head;
    qc_task_t              *task_tail;
    /* ⚠️ 队列长度必须记着，而且必须有上限。生产者（编码器线程写帧）通常
     * 远远快于消费者（loop 线程；一旦它卡住就更明显），无界队列会把进程的
     * 地址空间吃光 —— 实测连调试器都在里面 malloc 失败
     * （"address space is full"）。 */
    int                     task_count;

    /* 本客户端要 offer 的 ALPN（NULL = 用 tls_ctx_set_alpn 的写死默认）。
     * 存**副本**而不是借指针：quic_client_set_alpn 可能从任意线程调用，而真正
     * 生效是在 loop 线程执行 CONNECT 任务时 —— 中间调用方很可能已经释放了
     * 自己那份字符串。 */
    char                  **alpn;
    int                     alpn_count;

    /* 本轮连接的代号。DNS 解析是**异步**的，期间调用方完全可能已经 close 了 ——
     * 回调里靠它判断自己是不是过期了（见 on_resolved）。close 时自增。 */
    uint64_t                gen;
    /* 正在进行的解析（NULL = 没有）。close 时用来 uv_cancel，避免那个 request
     * 吊着 loop 不放（uv_loop_close 会返回 EBUSY）。 */
    struct qc_resolve      *resolve;
};

/* ── task queue (mutex) ────────────────────────── */

/* 4096 太宽了：每个写任务还拷着一整帧（关键帧几十 KB），4096 × 几十 KB
 * 就是上百 MB。1024 ≈ 8 秒的帧量，对「loop 短暂卡一下」已经绰绰有余。 */
#define QC_TASK_QUEUE_MAX 1024

static void task_push(quic_client_t *c, qc_task_t *t) {
    uv_mutex_lock(&c->task_mutex);
    /* ⚠️ 上限保护。生产者（编码器线程）通常远快于消费者（loop 线程），
     * 队列无界会把进程的地址空间吃光 —— 实测连调试器都在里面 malloc 失败
     * （"address space is full"）。满了就丢**新来的**；但 CLOSE **永远放行**，
     * 否则连接既发不出数据、也关不掉。 */
    if (t->type != QC_TASK_CLOSE && c->task_count >= QC_TASK_QUEUE_MAX) {
        static uint64_t dropped;
        dropped++;
        uv_mutex_unlock(&c->task_mutex);
        free(t->host);
        free(t->data);
        free(t);
        if (dropped == 1 || dropped % 1000 == 0) {
            LOG_WARN("[qc-api] task queue full (%d), dropped %llu tasks",
                     QC_TASK_QUEUE_MAX, (unsigned long long)dropped);
        }
        return;
    }
    t->next = NULL;
    if (!c->task_head) c->task_head = c->task_tail = t;
    else c->task_tail->next = t, c->task_tail = t;
    c->task_count++;
    uv_mutex_unlock(&c->task_mutex);
}

static qc_task_t *task_pop(quic_client_t *c) {
    uv_mutex_lock(&c->task_mutex);
    qc_task_t *t = c->task_head;
    if (t) {
        c->task_head = t->next;
        if (c->task_count > 0) c->task_count--;
    }
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

/* uv_close 完成回调：这里之后不会再有任何 QUIC 回调引用 c。 */
static void client_async_closed(uv_handle_t *h) {
    quic_client_t *c = (quic_client_t*)h->data;
    if (!c) return;

    /* 排队中的任务（close 之后不该再有，防御性回收） */
    uv_mutex_lock(&c->task_mutex);
    qc_task_t *t = c->task_head;
    c->task_head = c->task_tail = NULL;
    c->task_count = 0;
    uv_mutex_unlock(&c->task_mutex);
    while (t) {
        qc_task_t *next = t->next;
        free(t->host);
        free(t->data);
        free(t);
        t = next;
    }

    quic_stream_t *s = c->streams;
    while (s) {
        quic_stream_t *next = s->next;
        free(s);
        s = next;
    }
    if (c->alpn) {
        for (int i = 0; i < c->alpn_count; i++) free(c->alpn[i]);
        free(c->alpn);
    }
    uv_mutex_destroy(&c->task_mutex);
    free(c);
}

void quic_client_free(quic_client_t *c) {
    if (!c || c->freeing) return;
    c->freeing = 1;
    /* 不再向会话投递任何回调（连接已经关闭；这里只是兜底）。 */
    memset(&c->cb, 0, sizeof(c->cb));
    /* uv_close 的回调在下一次 loop 迭代执行；**必须在 loop 线程调用**。 */
    uv_close((uv_handle_t*)&c->async, client_async_closed);
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
 * DNS 解析 —— connect 之前把 host 解成 IPv4 字面量
 *
 * ⚠️ **为什么必须限 IPv4**：本仓 QUIC 的发送路径是写死的 IPv4 ——
 * quic_connection.c 里 `struct sockaddr_in addr; addr.sin_family = AF_INET;
 * uv_ip4_addr(conn->peer_ip_, …)`。DNS 若先返回 AAAA（很常见，getaddrinfo
 * 通常偏好 IPv6），那个 v6 字面量喂给 uv_ip4_addr 会解析失败、sockaddr 全零，
 * 包等于发到 0.0.0.0 —— **不报错**。所以 hints.ai_family = AF_INET。
 *
 * 刻意**不判断**「这是域名还是 IP 字面量」：uv_getaddrinfo 对数字字面量本来
 * 就立即返回（不走网络），一条路径两种情况都能吃 —— 少一处会写错的判断
 * （IPv6 方括号、inet_pton 的返回值、前导零…）。
 *
 * 解析是异步的，所以中间必然存在「还没连上但也没失败」的窗口。期间调用方
 * 可能 close —— 回调里靠 gen 判断自己是否过期（见 on_resolved）。
 * ============================================ */

typedef struct qc_resolve {
    quic_client_t   *c;
    qc_task_t       *task;      /* CONNECT 任务：解析收尾时才释放 */
    uint64_t         gen;       /* 发起时的代号 */
    uv_getaddrinfo_t req;
} qc_resolve_t;

static void on_resolved(uv_getaddrinfo_t *req, int status, struct addrinfo *res);

/* 建 SSL 参数、发起握手。ip 必须是 IPv4 字面量（解析那步保证了）。 */
static void qc_do_connect(quic_client_t *c, const char *ip, int port) {
    c->qc = QuicConnectionCreate(c->loop);
    if (!c->qc) {
        c->state = QC_STATE_IDLE;
        if (c->cb.on_connect) c->cb.on_connect(c, -1, c->cb.user_data);
        return;
    }
    QuicConnectionSetIdleTimeout(c->qc, 300000);
    QuicConnectionSetAppData(c->qc, c);
    QuicConnectionSetOnConnected(c->qc, on_quic_connected);
    QuicConnectionSetOnStreamData(c->qc, on_quic_stream_data);
    QuicConnectionSetOnStreamReset(c->qc, on_quic_stream_reset);
    QuicConnectionSetOnClose(c->qc, on_quic_close);
    /* ALPN 必须在 Connect 之前设 —— 它设的是已建好的 SSL 对象，握手一发起
     * offer 就发出去了。不设时保持 tls_ctx_set_alpn 的写死默认。 */
    if (c->alpn && c->alpn_count > 0) {
        QuicConnectionSetAlpnList(c->qc,
                                  (const char *const *)c->alpn,
                                  c->alpn_count);
    }
    QuicConnectionConnect(c->qc, ip, port);
}

/* 发起解析。返回 0 = request 已排上（task 由回调释放，调用方**不要**再 free），
 * <0 = 没发起（调用方照旧 free task）。 */
static int qc_start_resolve(quic_client_t *c, qc_task_t *t) {
    qc_resolve_t *r = (qc_resolve_t*)calloc(1, sizeof(*r));
    if (!r) return -1;
    r->c    = c;
    r->task = t;
    r->gen  = c->gen;
    r->req.data = r;

    char port[16];
    snprintf(port, sizeof(port), "%d", t->port);

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_INET;        /* ⚠️ 见上面的说明 */
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;

    int rc = uv_getaddrinfo(c->loop, &r->req, on_resolved, t->host, port, &hints);
    if (rc != 0) {
        LOG_WARN("[qc-api] uv_getaddrinfo(%s) failed: %s", t->host, uv_strerror(rc));
        free(r);
        return -1;
    }
    c->resolve = r;
    return 0;
}

static void on_resolved(uv_getaddrinfo_t *req, int status, struct addrinfo *res) {
    qc_resolve_t  *r = (qc_resolve_t*)req->data;
    quic_client_t *c = r->c;
    qc_task_t     *t = r->task;

    /* 过期判定：解析期间可能已经 close（gen 变了），或状态已经不是 CONNECTING。
     * 这两条都不能省 —— 过期了还去建连接，就是给一个已经关掉的会话装新连接。 */
    const int stale = (r->gen != c->gen) ||
                      (c->state != QC_STATE_CONNECTING) || c->closing;

    char ip[64] = {0};
    if (!stale && status == 0 && res) {
        for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
            if (ai->ai_family != AF_INET || !ai->ai_addr) continue;
            uv_ip4_name((const struct sockaddr_in*)ai->ai_addr, ip, sizeof(ip));
            break;
        }
    }
    if (res) uv_freeaddrinfo(res);
    if (c->resolve == r) c->resolve = NULL;

    if (stale) {
        free(t->host); free(t->data); free(t); free(r);
        return;
    }

    if (ip[0]) {
        LOG_INFO("[qc-api] resolved %s:%d -> %s", t->host, t->port, ip);
        qc_do_connect(c, ip, t->port);
    } else {
        /* 解析不出来要**明确报失败**，不能让调用方一直等 on_connect。 */
        LOG_WARN("[qc-api] cannot resolve \"%s\" (%s)", t->host,
                 status != 0 ? uv_strerror(status) : "no IPv4 address");
        c->state = QC_STATE_IDLE;
        if (c->cb.on_connect)
            c->cb.on_connect(c, status != 0 ? status : -1, c->cb.user_data);
    }
    free(t->host); free(t->data); free(t); free(r);
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
                c->gen++;                 /* 本轮连接的代号，close 时再自增 */
                /* 先解析再连。解析成功的话 task 交给 on_resolved 释放，
                 * 所以这里必须 continue —— 跳过循环尾部那次 free。 */
                if (qc_start_resolve(c, t) == 0) continue;
                /* 连解析都没排上（OOM / uv 报错）：立刻报失败，别让调用方干等。 */
                c->state = QC_STATE_IDLE;
                if (c->cb.on_connect)
                    c->cb.on_connect(c, -1, c->cb.user_data);
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
            /* 作废在途的解析：gen 一变，on_resolved 就会把自己判成过期并只做清理。
             * 另外 uv_cancel 一下，免得那个 request 吊着 loop（loop_close 会 EBUSY）。
             * ⚠️ 这里**不能**自己 free(r) —— 取消掉的 request 仍会回调一次，
             * 由回调释放。uv_cancel 返回 EBUSY 表示它已经完成、结果正在路上，
             * 那条路同样由回调的 gen 检查兜住。 */
            c->gen++;
            if (c->resolve) uv_cancel((uv_req_t*)&c->resolve->req);
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
    if (fin && c->cb.on_stream_fin)
        c->cb.on_stream_fin(c, s, c->cb.user_data);
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
