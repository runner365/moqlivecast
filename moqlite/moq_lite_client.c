/* ============================================================
 * moq-lite-06 客户端 —— C API 实现
 *
 * 承载：quic/quic_client_api（裸 QUIC，ALPN moq-lite-06）。
 * 线编码：moq_lite_wire.c（与 C++ 服务端 moq_lite_wire.hpp 逐字节一致）。
 *
 * ── 线程 ──
 * 全部工作在 quic_client_api 的 loop 线程里做。跨线程入口只有 quic_client_api
 * 自己的任务队列。所以：句柄查找（session_get/sub_get/bc_get）加锁，其余状态
 * 只在 loop 线程里碰。moq_lite_subscribe / moq_lite_session_announce 因此**必须
 * 在 loop 线程调**（头文件里写明了）。
 *
 * ── 订阅的时序（-06 的硬要求，最容易整个漏掉的一环）──
 *   ① 开 Track 流（bidi 0x6）问该 track 的不可变属性（priority/max_age/
 *      timescale）—— **与订阅无关，但必须先问**。参考实现的订阅端在有 track
 *      流时把状态机钉死在「先 Track、再 Subscribe」（lite/subscriber.rs 的
 *      EstablishState），不发 TRACK_INFO 它就一个 SUBSCRIBE 都不发。
 *   ② 读回 TRACK_INFO 之后才开 Subscribe 流（bidi 0x2）发 SUBSCRIBE。
 *   ③ 收 GROUP（uni 0x0）逐帧回调。
 *   **本端不发 SUBSCRIBE_END**：参考实现的订阅端只发 SUBSCRIBE_UPDATE，
 *   从不发 END（END 是**发布端**的表达）。订阅一直开到通话结束 / 我们主动
 *   reset 那条流。
 *
 * ── 本端角色 ──
 * 只做订阅方。SETUP 里带 Role=subscriber(2)，服务端据此只当发布端
 * （ArmPublisher），不会给我们发 ANNOUNCE_REQUEST（我们是订阅方，收到那个
 * 只会把流 reset 掉）。
 * ============================================================ */

#include "moq_lite_client.h"
#include "moq_lite_wire.h"
#include "quic_client_api.h"
#include "quic_connection.h"
#include "logger.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── -06 常量（与 moqlivecast/moq/moq_lite_session.cpp 那份一一对应）── */

enum {
    kUniSetup      = 0x1,
    kBidiAnnounce  = 0x1,
    kBidiSubscribe = 0x2,
    kBidiTrack     = 0x6,
};

/* 订阅应答 / announce 消息的类型判别符 */
enum {
    kRespStart = 0x0,
    kRespEnd   = 0x1,
    kRespDrop  = 0x2,
    kAnnStart  = 0x0,
    kAnnEnd    = 0x1,
};

enum { kParamPath = 0x2, kParamRole = 0x3 };

/* Role：**对端自己是谁**（moq_lite_role_t 的值直接用作线上编码）。 */
enum { kRoleBoth = 0, kRolePublisher = 1, kRoleSubscriber = 2 };

/* 流错误码（-06 §4.4.2）*/
enum { kStreamInternalError = 0x0, kStreamCancelled = 0x1 };

/* Subscriber Max Age：**0 是「零容忍」不是「不设上限」** —— 发 0 会让发布端
 * 把读游标钉在最新组上，任何组头已不在缓存的组整组跳过（音频实测少收 ~3.5%，
 * 浏览器表现为周期性 pop）。取参考实现的默认值 5s。完整推导见
 * moq_live_session.cpp 里 kSubMaxAgeMs 上面的长注释。 */
#define MOQ_SUB_MAX_AGE_MS 5000

/* SETUP hop 链上限（rs/model/origin.rs 的 MAX_HOPS=32）。 */
#define MOQ_MAX_HOPS 32

/* 单条接收流的缓冲上限（最大消息 + 余量）。对端声明巨大消息却不发完时靠它
 * 兜底：按**流**错误 reset，一条坏流不该把整个会话带走。 */
#define MOQ_MAX_RECV_BUF ((size_t)(MOQ_MAX_MESSAGE_SIZE) + 1024)

/* ============================================================
 * 内部结构
 * ============================================================ */

struct MoqSession;
struct MoqSub;
struct MoqBc;

typedef enum {
    kStreamSetup = 0,
    kStreamAnnounce,
    kStreamGroup,
    kStreamSubscribe,
    kStreamTrack,
} MoqStreamKind;

/* 一条接收流的解析状态。所有流共用这一个记录，按 kind 分派。
 * ⚠️ 解析必须**可重入**（每次数据到达都会再调一次），所有进度都存在这里。 */
typedef struct MoqStream {
    quic_stream_t *s;
    MoqStreamKind  kind;
    int            dead, done, fin, typed;
    int            local;      /* 本端开的（bidi）流：应答**不带** STREAM_TYPE */
    int            uni;        /* QUIC 单向流 */

    MoqBuf         buf;        /* 还没被消费掉的字节 */

    struct MoqSession *sess;
    struct MoqSub     *sub;    /* Subscribe / Track 流所属的订阅 */
    struct MoqPeerStream *peer;/* 对端开的请求流（announce / track）*/

    /* ── Group（uni 0x0）── */
    int      hdr_done;
    uint64_t sub_id_in_group;
    uint64_t group;
    uint64_t frame_index;
    int64_t  ts_acc;

    /* ── Announce（本端开的 bidi 0x1）── */
    int      ann_ok_done;
    uint64_t ann_expect;
    uint64_t ann_next_id;
    char    *req_prefix;
} MoqStream;

typedef enum {
    kPhaseTrackReq = 0,   /* 等 Track 流就绪 → 发 TRACK 请求 */
    kPhaseSubReq,         /* 等 Subscribe 流就绪 → 发 SUBSCRIBE */
    kPhaseFrames,         /* 收组 */
} MoqSubPhase;

typedef struct MoqSub {
    uint32_t          slot;     /* ctx->subs 下标，同时用作 subscribe id */
    int               used, dead;

    struct MoqSession *sess;
    struct MoqBc      *bc;
    uint64_t           sub_id;

    char              *track;
    moq_lite_sub_cb_t  cb;

    quic_stream_t     *sub_stream;
    quic_stream_t     *track_stream;

    int      phase;
    int      track_req_sent;
    int      sub_req_sent;
    int      started;        /* 收到 SUBSCRIBE_START */
    uint64_t groups;
    uint64_t frames;
} MoqSub;

typedef struct MoqBc {
    uint32_t   slot;
    int        used, dead;
    struct MoqSession *sess;
    char      *prefix;   /* 我们请求的 prefix */
    char      *suffix;   /* 对端报的 suffix */
    char      *path;     /* JoinPath(prefix, suffix) —— 线上广播全名 */
    uint64_t   ann_id;
} MoqBc;

/* ============================================================
 * 发布端（serve）方向 —— 我们是发布端，对端来订阅我们
 * ============================================================ */

typedef struct ServeFrame {
    uint64_t ts_us;
    uint8_t *data;
    size_t   len;
} ServeFrame;

/* 一条我们对外发布的轨。同时是 catalog 的一个 rendition、对端订阅时的查找
 * 目标、以及「写一帧」的入口。 */
typedef struct MoqPubTrack {
    uint32_t slot;
    int      used, dead;
    int      is_video;
    char    *name;
    char    *codec;
    uint8_t *config;        /* avcC / ASC 原字节（catalog 的 description 用）*/
    size_t   config_len;
    uint32_t width, height, sample_rate, channels, bitrate;

    /* ── 最近一段的保留缓冲 ──
     * 新订阅方**从这里起步**：视频从最近一个关键帧起（否则要等到下一个关键帧
     * 才有画面，而关键帧间隔可能十几秒），音频保留最近约 1 秒。
     *
     * ⚠️ 没有它的话，订阅方会落进「推流已经开始、但还没人来订阅」那段窗口 ——
     * 那段帧发给谁都不是（on-wire 没人收），视频那边这一等就是整整一个 GOP，
     * 于是**视频永久落后音频一个 GOP**；下游（服务端的 egress / 浏览器）看到
     * 的音视频时间轴错开，视频干脆不显示。 */
    ServeFrame *hold;
    size_t      hold_cnt, hold_cap;
    uint64_t    hold_bytes;

    struct MoqPub *pub;
} MoqPubTrack;

/* 一条我们对外发布的广播（= 一个 catalog + N 条轨）。
 *
 * 组号**按订阅**编、不按轨：同一条轨的两个订阅方必须看到同一批组号，否则各自
 * 的时间轴对不上。所以组状态在 MoqServeSub 上，不在这里。 */
#define MOQ_MAX_PUB_TRACKS 4
typedef struct MoqPub {
    uint32_t slot;
    int      used, dead;
    char    *path;          /* 线上广播名 */
    struct MoqSession *sess;
    MoqPubTrack *tracks[MOQ_MAX_PUB_TRACKS];
    int      track_cnt;
    MoqBuf   catalog;       /* 拼好的 catalog.json（轨声明后惰性重建）*/
} MoqPub;

/* 对端订阅了我们某条轨。
 *
 * ⚠️ sid / sub_stream 是**对端选的 subscribe id 与对端开的那条 bidi 流**，
 * 与我们开给对方的那条**组流**是两回事 —— 组流头里带的必须是这个 sid。 */
typedef struct MoqServeSub {
    uint32_t slot;
    int      used, dead;
    uint64_t sid;              /* 对端选的 subscribe id */
    quic_stream_t *sub_stream; /* 对端开的 bidi；SUBSCRIBE_START 写回这里 */
    MoqPubTrack  *track;
    struct MoqSession *sess;

    int      started;          /* SUBSCRIBE_START 已发 */
    uint64_t group_seq;        /* 本订阅内递增，从 0 起 */
    quic_stream_t *uni;        /* 当前开着的组流（NULL = 没开）*/
    int64_t  group_prev_us;    /* 组内上一帧的绝对微秒（差值的累加基线）*/
    int64_t  group_open_us;
    int      group_had_media;
    int64_t  next_audio_us;    /* 音频按秒切组的分界点 */
    uint64_t frames;

    /* ── 组的开出是**异步**的 ──
     * quic_client_open_uni_stream 之后要等回调才拿得到 stream 句柄。这中间
     * 到达的帧既不能丢（丢的就是组的第一帧 —— 视频那边**就是关键帧**）、也
     * 不能提前写（组头还没发出去），所以先攒起来，组头写完再按序补。 */
    int         group_opening; /* 已发起开流、还没拿到句柄 */
    ServeFrame *pend;
    size_t      pend_cnt, pend_cap;

    /* catalog.json 这条订阅专用：载荷是**裸 JSON**（没有容器时间戳前缀），
     * 借用所属 MoqPub 的 catalog 缓冲（它的寿命比订阅长）。 */
    const uint8_t *catalog_json;
    size_t         catalog_len;
} MoqServeSub;

/* 对端开的一条**请求**流（announce / track）。应答写回**同一条**流，所以流 id
 * 本身就是关联键，不需要另做 ID 配对。 */
typedef struct MoqPeerStream {
    uint32_t slot;
    int      used, dead;
    struct MoqSession *sess;
    quic_stream_t *in;         /* 对端开的这条 bidi */
    int      wire_type;        /* kBidiAnnounce / kBidiTrack */
    int      reply_sent;
    MoqBuf   reply;            /* 待写的应答字节（开流是异步的，先编码后发）*/
} MoqPeerStream;

/* 请求流的解析状态放在 MoqStream 上（每个 kind 自己的字段），这里只给一个
 * 反向指针把两边连起来。 */

typedef struct MoqSession {
    uint32_t slot;
    int      used, dead, closing;
    int      connecting, connected;

    char    *host;   /* 仅日志用 */
    int      port;
    char    *path;   /* URL 的路径部分，决定服务端路由 */

    moq_lite_session_cb_t cb;

    quic_client_t  *cli;
    uv_loop_t      *loop;

    quic_stream_t  *setup_stream;
    int             setup_sent;
    quic_stream_t  *announce_stream;
    int             announce_sent;

    MoqStream     **streams;
    size_t          stream_cnt, stream_cap;

    MoqBc         **bcs;
    size_t          bc_cnt, bc_cap;

    /* ── 发布端方向 ──
     * role 写进 SETUP 的 Role 参数（见 moq_lite_session_set_role）。 */
    int             role;
    MoqPub        **pubs;
    size_t          pub_cnt, pub_cap;
    MoqServeSub   **serve_subs;
    size_t          serve_cnt, serve_cap;
    MoqPeerStream **peers;
    size_t          peer_cnt, peer_cap;
} MoqSession;

struct MoqPub;
struct MoqPubTrack;

/* 进程内全局状态 —— 注册表是单例（句柄是整数，得有个进程级的表来解引用），
 * 但**事件循环不是**：每条会话各自带调用方给的 loop，见 MoqSession::loop。
 * ⚠️ 这带来一个真实限制：注册表是进程级、而 loop 是调用方级的，所以
 * **多个 loop 上各跑一条会话时，session_get/sub_get 的锁是共享的**（那部分
 * 没问题），但 `g_pending`（开流意图队列）是单例、不带 loop 维度 —— 目前
 * 只有单 loop 场景在用。要用多 loop 就得把它挪进 MoqSession。 */
typedef struct {
    MoqSession    **sessions;
    size_t          session_cnt, session_cap;
    MoqSub        **subs;
    size_t          sub_cnt, sub_cap;
    MoqBc         **bcs;
    size_t          bc_cnt, bc_cap;

    /* 发布侧的两张全局表。句柄（moq_lite_broadcast_t / moq_lite_track_t）是
     * 进程级的，所以要有个进程级的表能按句柄反查 —— 光挂在会话上不行
     * （会话是按 loop 的，句柄不带会话信息）。 */
    struct MoqPub      **pubs;
    size_t               pub_cnt, pub_cap;
    struct MoqPubTrack **tracks;
    size_t               track_cnt, track_cap;

    char          **alpn;       /* set_alpn 设的进程级覆盖 */
    int             alpn_count;
} MoqCtx;

static MoqCtx g_ctx;
static pthread_mutex_t g_ctx_mutex = PTHREAD_MUTEX_INITIALIZER;

/* 线程局部的错误描述 */
static __thread char g_err[256] = "ok";

static int err_set(int code, const char *msg) {
    snprintf(g_err, sizeof(g_err), "%s", msg ? msg : "(no detail)");
    return code;
}

const char *moq_lite_strerror(int err) {
    switch (err) {
    case MOQ_LITE_OK:          return "ok";
    case MOQ_LITE_ERR_ARGS:    return "invalid handle or argument";
    case MOQ_LITE_ERR_URL:     return "unsupported or malformed url";
    case MOQ_LITE_ERR_NOMEM:   return "out of memory";
    case MOQ_LITE_ERR_STATE:   return "session not in a state that allows this";
    case MOQ_LITE_ERR_INTERNAL:return "internal error";
    default:                   return g_err[0] ? g_err : "unknown error";
    }
}

/* ============================================================
 * 容器增长 / 句柄查找
 * ============================================================ */

static int ensure_slot(void ***arr, size_t *cap, size_t need, size_t elt) {
    if (need < *cap) return 0;
    size_t want = *cap ? *cap * 2 : 8;
    while (want <= need) want *= 2;
    void *nb = realloc(*arr, want * elt);
    if (!nb) return -1;
    *arr = nb;
    *cap = want;
    return 0;
}

/* 分配一个句柄槽位。**0 号恒留给「无效句柄」**（与 libmoq 的约定一致）——
 * 所以下标 0 永远是个占位 NULL，真正的第一个对象从 1 号开始。 */
static size_t alloc_slot(void ***arr, size_t *cnt, size_t *cap, void *obj,
                         size_t elt) {
    if (*cnt == 0) {
        if (ensure_slot(arr, cap, 0, elt) < 0) return (size_t)-1;
        (*arr)[0] = NULL;
        *cnt = 1;
    }
    if (ensure_slot(arr, cap, *cnt, elt) < 0) return (size_t)-1;
    size_t slot = *cnt;
    (*arr)[slot] = obj;
    *cnt += 1;
    return slot;
}

/* 这三个可能从任意线程调（close / unsubscribe 明确允许跨线程），故加锁 —
 * 指针回收发生在 loop 线程，所以这里只读不写。 */
static MoqSession *session_get(moq_lite_session_t h) {
    MoqSession *s = NULL;
    pthread_mutex_lock(&g_ctx_mutex);
    if (h < g_ctx.session_cnt && g_ctx.sessions[h] &&
        g_ctx.sessions[h]->used && !g_ctx.sessions[h]->dead &&
        g_ctx.sessions[h]->slot == h)
        s = g_ctx.sessions[h];
    pthread_mutex_unlock(&g_ctx_mutex);
    return s;
}

static MoqSub *sub_get(moq_lite_sub_t h) {
    MoqSub *s = NULL;
    pthread_mutex_lock(&g_ctx_mutex);
    if (h < g_ctx.sub_cnt && g_ctx.subs[h] &&
        g_ctx.subs[h]->used && g_ctx.subs[h]->slot == h)
        s = g_ctx.subs[h];
    pthread_mutex_unlock(&g_ctx_mutex);
    return s;
}

static MoqBc *bc_get(moq_lite_broadcast_t h) {
    MoqBc *b = NULL;
    pthread_mutex_lock(&g_ctx_mutex);
    if (h < g_ctx.bc_cnt && g_ctx.bcs[h] &&
        g_ctx.bcs[h]->used && !g_ctx.bcs[h]->dead && g_ctx.bcs[h]->slot == h)
        b = g_ctx.bcs[h];
    pthread_mutex_unlock(&g_ctx_mutex);
    return b;
}

/* 发布侧的句柄反查。与上面几个同构，但**发布侧的写入一律在 loop 线程**
 * （_publish / _publish_track / _track_frame 都要求 loop 线程），所以这里
 * 加锁只是为了和读侧一致，不是防并发写。 */
static MoqPub *pub_get(moq_lite_broadcast_t h) {
    MoqPub *p = NULL;
    if (h == 0) return NULL;
    pthread_mutex_lock(&g_ctx_mutex);
    if (h < g_ctx.pub_cnt && g_ctx.pubs[h] &&
        g_ctx.pubs[h]->used && !g_ctx.pubs[h]->dead &&
        g_ctx.pubs[h]->slot == h)
        p = g_ctx.pubs[h];
    pthread_mutex_unlock(&g_ctx_mutex);
    return p;
}

static MoqPubTrack *track_get(moq_lite_track_t h) {
    MoqPubTrack *t = NULL;
    if (h == 0) return NULL;
    pthread_mutex_lock(&g_ctx_mutex);
    if (h < g_ctx.track_cnt && g_ctx.tracks[h] &&
        g_ctx.tracks[h]->used && !g_ctx.tracks[h]->dead &&
        g_ctx.tracks[h]->slot == h)
        t = g_ctx.tracks[h];
    pthread_mutex_unlock(&g_ctx_mutex);
    return t;
}

/* ============================================================
 * URL 解析
 * ============================================================ */

/* 只认裸 QUIC 的两个方案名。https:// 是 WebTransport 那条路，本库不做 ——
 * 明确报错而不是静默降级：静默降级会变成「连上了但没下文」。 */
static int parse_url(const char *url, char **host, int *port, char **path) {
    if (!url) return err_set(MOQ_LITE_ERR_URL, "url is null");

    const char *p = url;
    if (strncmp(p, "moql://", 7) == 0)           p += 7;
    else if (strncmp(p, "moq-lite://", 11) == 0) p += 11;
    else if (strncmp(p, "https://", 8) == 0)
        return err_set(MOQ_LITE_ERR_URL,
                       "https:// is WebTransport (h3); this library speaks bare "
                       "QUIC only - use moql://");
    else
        return err_set(MOQ_LITE_ERR_URL, "url must start with moql://");

    if (!*p) return err_set(MOQ_LITE_ERR_URL, "url has no host");

    const char *slash = strchr(p, '/');
    const char *host_end = slash ? slash : p + strlen(p);
    const char *colon = NULL;

    /* 端口分隔符只看 host 段：IPv6 的 [::1]:4433 里那个冒号在方括号内，不算。 */
    if (*p == '[') {
        const char *rb = memchr(p, ']', (size_t)(host_end - p));
        if (!rb) return err_set(MOQ_LITE_ERR_URL, "unterminated IPv6 literal");
        if (rb + 1 < host_end && rb[1] == ':') colon = rb + 1;
    } else {
        for (const char *c = p; c < host_end; c++) if (*c == ':') colon = c;
    }

    size_t hlen = (size_t)((colon ? colon : host_end) - p);
    if (hlen == 0 || hlen > 255) return err_set(MOQ_LITE_ERR_URL, "bad host");

    *host = strndup(p, hlen);
    *path = strdup(slash ? slash : "");
    if (!*host || !*path) {
        free(*host); free(*path);
        *host = NULL; *path = NULL;
        return err_set(MOQ_LITE_ERR_NOMEM, "oom");
    }

    *port = MOQ_LITE_DEFAULT_PORT;
    if (colon) {
        int v = atoi(colon + 1);
        if (v <= 0 || v > 65535) {
            free(*host); free(*path); *host = NULL; *path = NULL;
            return err_set(MOQ_LITE_ERR_URL, "bad port");
        }
        *port = v;
    }
    return MOQ_LITE_OK;
}

/* ============================================================
 * 流记录
 * ============================================================ */

static MoqStream *stream_find(MoqSession *s, quic_stream_t *st) {
    for (size_t i = 0; i < s->stream_cnt; i++)
        if (s->streams[i] && s->streams[i]->s == st) return s->streams[i];
    return NULL;
}

static MoqStream *stream_add(MoqSession *s, quic_stream_t *st, int kind,
                             int local) {
    MoqStream *r = (MoqStream*)calloc(1, sizeof(*r));
    if (!r) return NULL;
    if (alloc_slot((void***)&s->streams, &s->stream_cnt, &s->stream_cap,
                   r, sizeof(MoqStream*)) == (size_t)-1) {
        free(r);
        return NULL;
    }
    r->s      = st;
    r->kind   = kind;
    r->local  = local;
    r->uni    = quic_client_stream_is_uni(st);
    r->sess   = s;
    r->typed  = local ? 1 : 0;   /* 本端开的流：类型字节是我们自己写的 */
    moq_buf_init(&r->buf);
    return r;
}

/* 回收一条收尾的流记录。
 *
 * ⚠️ **必须回收**：一条组流一个记录，而 moq-lite 是「一个 group 一条 uni 流」
 * —— 音频约 47 条/秒、视频每几秒一条。不回收的话记录数组和它各自的缓冲会
 * 无界增长（跑一小时音频 ≈ 17 万个记录）。
 * 只在**解析返回之后**调（解析过程中 r 还在用）。 */
static void stream_remove(MoqSession *s, MoqStream *r) {
    for (size_t i = 0; i < s->stream_cnt; i++) {
        if (s->streams[i] != r) continue;
        s->streams[i] = s->streams[s->stream_cnt - 1];
        s->stream_cnt--;
        moq_buf_free(&r->buf);
        free(r->req_prefix);
        free(r);
        return;
    }
}

/* 写 STREAM_TYPE + 一条消息到**本端新开**的流。控制流不能 FIN（要一直开着
 * 等应答），所以 fin 由调用方定。 */
static int write_ctrl(MoqSession *s, quic_stream_t *st, uint64_t stream_type,
                      int typed, uint64_t type, const MoqBuf *body, int fin) {
    (void)s;
    if (!st) return MOQ_LITE_ERR_STATE;
    MoqBuf out;
    moq_buf_init(&out);
    int rc = MOQ_LITE_OK;
    if (moq_put_stream_type(&out, stream_type) < 0 ||
        moq_msg_build(&out, typed, type, body ? body->buf : NULL,
                      body ? body->len : 0) < 0) {
        rc = MOQ_LITE_ERR_NOMEM;
    } else {
        quic_client_stream_write(st, out.buf, out.len, fin, NULL, NULL, 0);
    }
    moq_buf_free(&out);
    return rc;
}

/* SETUP：STREAM_TYPE(0x1) + [len][param count] + params，发完 FIN（一次性）。*/
static void session_send_setup(MoqSession *s) {
    if (s->setup_sent || !s->setup_stream) return;
    s->setup_sent = 1;

    MoqBuf params, body;
    moq_buf_init(&params);
    moq_buf_init(&body);
    uint64_t count = 0;
    int rc = MOQ_LITE_OK;

    do {
        /* Path —— 服务端据此路由。这条参数**只有客户端发**（服务端发才是
         * 违规），空值等价于没发。实测 moq-cli 的 SETUP 不带它，broadcast
         * 命名空间根在 ""，所以路由只能靠这条。 */
        if (s->path && s->path[0]) {
            MoqBuf v; moq_buf_init(&v);
            if (moq_str_write(&v, s->path) < 0 ||
                moq_varint_write(&params, kParamPath) < 0 ||
                moq_varint_write(&params, v.len) < 0 ||
                moq_buf_append(&params, v.buf, v.len) < 0) {
                rc = MOQ_LITE_ERR_NOMEM;
                moq_buf_free(&v);
                break;
            }
            moq_buf_free(&v);
            count++;
        }

        /* Role：**对端自己是谁**（不是我们是谁）。默认 2（订阅方）：服务端据此
         * 只当发布端，不会给我们发 ANNOUNCE_REQUEST —— 那样会白白换来一条被
         * reset 的流（我们没实现应答时确实如此）。
         * moq_lite_session_set_role(PUBLISHER=1) 时发 1：对端于是当订阅方，
         * 发 ANNOUNCE_REQUEST 来订阅我们。 */
        if (moq_varint_write(&params, kParamRole) < 0 ||
            moq_varint_write(&params, 1) < 0 ||
            moq_varint_write(&params, (uint64_t)s->role) < 0) {
            rc = MOQ_LITE_ERR_NOMEM;
            break;
        }
        count++;

        if (moq_varint_write(&body, count) < 0 ||
            moq_buf_append(&body, params.buf, params.len) < 0)
            rc = MOQ_LITE_ERR_NOMEM;
    } while (0);
    moq_buf_free(&params);

    if (rc != MOQ_LITE_OK) {
        moq_buf_free(&body);
        LOG_ERROR("[moq-lite-cli] cannot encode setup");
        return;
    }

    MoqBuf out;
    moq_buf_init(&out);
    if (moq_put_stream_type(&out, kUniSetup) < 0 ||
        moq_msg_build(&out, 0, 0, body.buf, body.len) < 0) {
        LOG_ERROR("[moq-lite-cli] cannot build setup");
    } else {
        quic_client_stream_write(s->setup_stream, out.buf, out.len, 1,
                                 NULL, NULL, 0);
        LOG_INFO("[moq-lite-cli] SETUP sent: path=\"%s\" params=%llu "
                 "role=subscriber",
                 (s->path && s->path[0]) ? s->path : "(default)",
                 (unsigned long long)count);
    }
    moq_buf_free(&out);
    moq_buf_free(&body);
}

/* ANNOUNCE_REQUEST（普通消息，无 Type 判别符；body 只有一个 (s) 前缀）。
 * 前缀是**绝对**路径；空串 = 「本 scope 下的全部」，也是最常见的用法。 */
static void session_send_announce(MoqSession *s, const char *prefix) {
    if (!s->announce_stream || s->announce_sent) return;

    MoqStream *r = stream_find(s, s->announce_stream);
    if (r) { free(r->req_prefix); r->req_prefix = strdup(prefix ? prefix : ""); }

    MoqBuf body;
    moq_buf_init(&body);
    if (moq_str_write(&body, prefix ? prefix : "") < 0 ||
        write_ctrl(s, s->announce_stream, kBidiAnnounce, 0, 0, &body, 0) !=
            MOQ_LITE_OK) {
        LOG_ERROR("[moq-lite-cli] cannot send announce request");
        quic_client_stream_reset(s->announce_stream, kStreamInternalError);
    } else {
        s->announce_sent = 1;
        LOG_INFO("[moq-lite-cli] ANNOUNCE_REQUEST sent: prefix=\"%s\"",
                 (prefix && prefix[0]) ? prefix : "(all)");
    }
    moq_buf_free(&body);
}

/* 全路径 = 请求前缀 + 路由后缀，中间补一个 '/'（空的一侧不补）。
 * 与 C++ 服务端的 JoinPath 同规则。 */
static char *join_path(const char *prefix, const char *suffix) {
    const char *a = prefix ? prefix : "";
    const char *b = suffix ? suffix : "";
    if (!*a) return strdup(b);
    if (!*b) return strdup(a);
    size_t la = strlen(a), lb = strlen(b);
    char *out = (char*)malloc(la + lb + 2);
    if (!out) return NULL;
    memcpy(out, a, la);
    out[la] = '/';
    memcpy(out + la + 1, b, lb);
    out[la + 1 + lb] = '\0';
    return out;
}

/* ============================================================
 * 订阅
 * ============================================================ */

static void sub_pump(MoqSub *sub);

/* 开流意图队列 —— 定义在下面（quic_client 回调那一节），但订阅侧要用它，
 * 所以这里先声明。 */
typedef enum {
    kOpenSetup = 1,     /* uni：SETUP 流 */
    kOpenAnnounce,      /* bidi：本端发起发现 */
    kOpenTrack,         /* bidi：本端发起 Track 请求 */
    kOpenSubscribe,     /* bidi：本端发起订阅 */
    kOpenTrackGroup,    /* uni：给某条订阅开组流 */
    kOpenAnnounceReply, /* bidi：应答对端的 announce 请求（用同一条流）*/
    kOpenTrackInfo,     /* bidi：应答对端的 track 请求（TRACK_INFO + FIN）*/
} OpenKind;
static void pending_push(OpenKind kind, MoqSub *sub);
static void pending_push_box(OpenKind kind, void *box);

/* ⚠️ 开流与「登记这次开流是给谁的」必须成对 —— 回调里只拿得到
 * quic_stream_t，靠堆栈弹出的意图才知道该挂到哪个订阅上。漏了 push 的后果是
 * 回调被判成「unsolicited」、流被 reset，然后订阅永远等不到应答。
 * 所以这两个函数**自己**做 push，调用方只要调它们就行。 */
static void sub_open_track(MoqSub *sub) {
    if (sub->track_stream || sub->dead) return;
    pending_push(kOpenTrack, sub);
    quic_client_open_stream(sub->sess->cli);
}

static void sub_open_subscribe(MoqSub *sub) {
    if (sub->sub_stream || sub->dead) return;
    pending_push(kOpenSubscribe, sub);
    quic_client_open_stream(sub->sess->cli);
}

static void sub_pump(MoqSub *sub) {
    MoqSession *s = sub->sess;
    if (sub->dead || s->dead) return;

    if (sub->phase == kPhaseTrackReq) {
        if (!sub->track_stream || sub->track_req_sent) return;
        /* Track 流（bidi 0x6）上的请求：普通消息，body = broadcast(s) + track(s)。*/
        MoqBuf body;
        moq_buf_init(&body);
        int rc = MOQ_LITE_OK;
        if (moq_str_write(&body, sub->bc ? sub->bc->path : "") < 0 ||
            moq_str_write(&body, sub->track) < 0)
            rc = MOQ_LITE_ERR_NOMEM;
        if (rc == MOQ_LITE_OK)
            rc = write_ctrl(s, sub->track_stream, kBidiTrack, 0, 0, &body, 0);

        if (rc != MOQ_LITE_OK) {
            LOG_ERROR("[moq-lite-cli] sub %u: cannot send track request",
                      sub->sub_id);
            quic_client_stream_reset(sub->track_stream, kStreamInternalError);
            /* 拿不到 TRACK_INFO 也要能继续订阅：那里面只有 timescale 与
             * priority，而我们的数据是自描述（内层微秒前缀），不依赖它。 */
            sub->phase = kPhaseSubReq;
            sub_open_subscribe(sub);
        } else {
            sub->track_req_sent = 1;
            LOG_INFO("[moq-lite-cli] sub %u: TRACK request sent: broadcast=\"%s\" "
                     "track=\"%s\"", sub->sub_id, sub->bc ? sub->bc->path : "",
                     sub->track);
        }
        moq_buf_free(&body);
        return;
    }

    if (sub->phase == kPhaseSubReq) {
        if (!sub->sub_stream || sub->sub_req_sent) return;
        /* SUBSCRIBE body（顺序即线上顺序，一处都不能错）：
         *   id(i) broadcast(s) track(s) priority(u8) max_age(i)
         *   start_group(i) end_group(i) start_frame(i) end_frame(i)
         * ⚠️ 三处易错：① -06 **没有** group-order 那个字节（01~05 专有，
         * 多一个字节后面全错位）；② priority 是**单字节 u8** 不是 varint；
         * ③ start_* 的 0 = 「不限」，end_* 是 N+1 形式、0 也 = 「不限」。
         * 我们全 0 = 整条 track 不设限。 */
        MoqBuf body;
        moq_buf_init(&body);
        int rc = MOQ_LITE_OK;
        do {
            if (moq_varint_write(&body, sub->sub_id) < 0)             { rc = -1; break; }
            if (moq_str_write(&body, sub->bc ? sub->bc->path : "") < 0) { rc = -1; break; }
            if (moq_str_write(&body, sub->track) < 0)                 { rc = -1; break; }
            if (moq_buf_append(&body, "\x00", 1) < 0)                 { rc = -1; break; }
            if (moq_varint_write(&body, MOQ_SUB_MAX_AGE_MS) < 0)      { rc = -1; break; }
            if (moq_varint_write(&body, 0) < 0)                       { rc = -1; break; }
            if (moq_varint_write(&body, 0) < 0)                       { rc = -1; break; }
            if (moq_varint_write(&body, 0) < 0)                       { rc = -1; break; }
            if (moq_varint_write(&body, 0) < 0)                       { rc = -1; break; }
        } while (0);

        if (rc < 0)
            rc = MOQ_LITE_ERR_NOMEM;
        else
            rc = write_ctrl(s, sub->sub_stream, kBidiSubscribe, 0, 0, &body, 0);

        if (rc != MOQ_LITE_OK) {
            LOG_ERROR("[moq-lite-cli] sub %u: cannot send subscribe", sub->sub_id);
            quic_client_stream_reset(sub->sub_stream, kStreamInternalError);
        } else {
            sub->sub_req_sent = 1;
            sub->phase = kPhaseFrames;
            LOG_INFO("[moq-lite-cli] sub %u: SUBSCRIBE sent: broadcast=\"%s\" "
                     "track=\"%s\" max_age=%dms", sub->sub_id,
                     sub->bc ? sub->bc->path : "", sub->track,
                     MOQ_SUB_MAX_AGE_MS);
        }
        moq_buf_free(&body);
        return;
    }
}

/* ============================================================
 * 帧投递
 * ============================================================ */

/* legacy 容器的媒体帧在载荷最前面垫了一个**绝对微秒**的 varint 时间戳
 * （实测：视频 `80 99 88 b0` = 10062000 us）；**catalog 这类数据轨没有这一层**
 * （JSON 从第 0 字节起）。
 *
 * 库里**不做这个猜测** —— 剥错是**静默**的（catalog 的 JSON 以 `{"` 开头，
 * `0x7B` 正好是个合法的 2 字节 varint 首字节，剥掉就少 2 个字节，打印出来
 * 只是「开头少了两个字符」，很难联想到偏移）。谁订阅的谁知道该不该剥，所以
 * 解析交给调用方，见 moq_lite_strip_container_ts。 */
int moq_lite_strip_container_ts(const uint8_t *payload, size_t len,
                                uint64_t *ts_us, const uint8_t **out,
                                size_t *out_len) {
    if (!payload || len == 0) return MOQ_LITE_ERR_ARGS;
    size_t off = 0;
    uint64_t v = 0;
    if (moq_varint_read(payload, len, &off, &v) != 0) return MOQ_LITE_ERR_ARGS;
    if (ts_us)   *ts_us   = v;
    if (out)     *out     = payload + off;
    if (out_len) *out_len = len - off;
    return MOQ_LITE_OK;
}

static void sub_deliver_frame(MoqSub *sub, uint64_t group, uint64_t frame_index,
                              int64_t ts_delta, const uint8_t *payload,
                              size_t len) {
    if (len == 0) return;

    /* 唯一一处需要看前缀的地方：**空载荷哨兵**。
     * 它的线上形状是「只有时间戳前缀、0 字节编解码载荷」（参考实现
     * fn end() = frame.payload.is_empty().then_some(ts) —— **解出时间戳之后**
     * 载荷为空才算），占一个帧槽但不是媒体帧，所以在这里丢掉。
     * 这不会误伤数据轨：catalog 的 JSON 远长于一个 varint，剥完不可能为空。 */
    {
        size_t off = 0;
        uint64_t v = 0;
        if (moq_varint_read(payload, len, &off, &v) == 0 && off >= len) {
            LOG_DEBUG("[moq-lite-cli] sub %u: end-of-group marker at %llu us",
                      sub->sub_id, (unsigned long long)v);
            return;
        }
    }

    /* ⚠️ 载荷**原样**交出去（含容器前缀）。要剥的调用方用
     * moq_lite_strip_container_ts。 */
    sub->frames++;
    if (sub->cb.on_frame)
        sub->cb.on_frame(sub->slot, group, frame_index, ts_delta, payload, len,
                         sub->cb.user);
}

/* ============================================================
 * 解析
 * ============================================================ */

static MoqSub *session_sub_by_id(MoqSession *s, uint64_t sub_id) {
    /* 只在 loop 线程调（g_ctx.subs 的写入也都在 loop 线程），不必加锁。 */
    for (size_t i = 0; i < g_ctx.sub_cnt; i++) {
        MoqSub *sub = g_ctx.subs[i];
        if (sub && sub->used && sub->sess == s && sub->sub_id == sub_id) return sub;
    }
    return NULL;
}

/* 消费掉 buf 的前 n 字节。 */
static void buf_consume(MoqBuf *b, size_t n) {
    if (n >= b->len) { b->len = 0; return; }
    memmove(b->buf, b->buf + n, b->len - n);
    b->len -= n;
}

static void parse_group(MoqStream *r) {
    MoqSession *s = r->sess;

    if (!r->hdr_done) {
        size_t off = 0, blen = 0;
        MoqMsgStatus st = moq_msg_read(r->buf.buf, r->buf.len, &off, &blen);
        if (st == MOQ_MSG_TOO_LARGE) {
            quic_client_stream_reset(r->s, kStreamInternalError);
            r->done = 1;
            return;
        }
        if (st == MOQ_MSG_NEED_MORE) {
            if (r->fin) r->done = 1;
            return;
        }
        const size_t end = off + blen;
        size_t pos = off;
        uint64_t sub_id = 0, seq = 0, frame_start = 0;
        if (moq_varint_read(r->buf.buf, end, &pos, &sub_id) < 0 ||
            moq_varint_read(r->buf.buf, end, &pos, &seq) < 0 ||
            /* ⚠️ frame_start 是 06 新增的第三个 varint。少读这一个，后面第一个
             * FRAME 的时间戳差就整个解错 —— 而且**不报错**。 */
            moq_varint_read(r->buf.buf, end, &pos, &frame_start) < 0 ||
            pos != end) {
            LOG_WARN("[moq-lite-cli] malformed group header");
            quic_client_stream_reset(r->s, kStreamInternalError);
            r->done = 1;
            return;
        }
        if (!session_sub_by_id(s, sub_id)) {
            /* 没订阅过的 Subscribe ID：按「已取消」处理，**不**升级成会话错误。
             * 典型场景：我们自己 unsubscribe 后对端还在发的旧组流。 */
            LOG_DEBUG("[moq-lite-cli] group for unknown subscribe id %llu — "
                      "resetting", (unsigned long long)sub_id);
            quic_client_stream_reset(r->s, kStreamCancelled);
            r->done = 1;
            return;
        }
        r->hdr_done = 1;
        r->sub_id_in_group = sub_id;
        r->group = seq;
        r->frame_index = frame_start;
        r->ts_acc = 0;
        buf_consume(&r->buf, end);

        LOG_DEBUG("[moq-lite-cli] GROUP: sub=%llu group=%llu frame_start=%llu",
                  (unsigned long long)sub_id, (unsigned long long)seq,
                  (unsigned long long)frame_start);
        MoqSub *sub = session_sub_by_id(s, sub_id);
        if (sub) sub->groups++;
    }

    /* FRAME 序列：背靠背，**没有类型字节**。 */
    for (;;) {
        size_t off = 0;
        uint64_t zz = 0;
        if (moq_varint_read(r->buf.buf, r->buf.len, &off, &zz) < 0) break;
        size_t boff = off, blen = 0;
        MoqMsgStatus st = moq_msg_read(r->buf.buf, r->buf.len, &boff, &blen);
        if (st == MOQ_MSG_TOO_LARGE) {
            quic_client_stream_reset(r->s, kStreamInternalError);
            r->done = 1;
            return;
        }
        if (st == MOQ_MSG_NEED_MORE) break;

        r->ts_acc += moq_zigzag_decode(zz);
        MoqSub *sub = session_sub_by_id(s, r->sub_id_in_group);
        if (sub)
            sub_deliver_frame(sub, r->group, r->frame_index,
                              moq_zigzag_decode(zz), r->buf.buf + boff, blen);

        /* ⚠️ 必须**投递之后**再前移缓冲：payload 指向 r->buf 内部。 */
        buf_consume(&r->buf, boff + blen);
        r->frame_index++;
    }

    if (r->fin) {
        if (r->buf.len)
            LOG_WARN("[moq-lite-cli] group stream ended mid-frame, %zu trailing "
                     "bytes dropped", r->buf.len);
        r->done = 1;
    }
}

static void parse_announce(MoqStream *r) {
    MoqSession *s = r->sess;
    const char *prefix = r->req_prefix ? r->req_prefix : "";

    if (!r->ann_ok_done) {
        size_t off = 0, blen = 0;
        MoqMsgStatus st = moq_msg_read(r->buf.buf, r->buf.len, &off, &blen);
        if (st == MOQ_MSG_TOO_LARGE) {
            quic_client_stream_reset(r->s, kStreamInternalError);
            return;
        }
        if (st == MOQ_MSG_NEED_MORE) {
            if (r->fin) {
                /* 对端 FIN 在这条流上是**正常收尾**（它没什么可公告的），
                 * 参考实现不把它升级成会话错误。 */
                LOG_INFO("[moq-lite-cli] announce closed by peer before OK "
                         "(prefix=\"%s\", clean end)", prefix[0] ? prefix : "(all)");
                r->done = 1;
            }
            return;
        }
        const size_t end = off + blen;
        size_t pos = off;
        uint64_t hop = 0, active = 0;
        /* ⚠️ 字段顺序是 **Hop ID 在前、Active Count 在后**（-06 §7.5）。反了的话
         * active 会读成 hop，而两者都是小整数 —— 静默错，只能靠日志里数字不对
         * 才看出来。 */
        if (moq_varint_read(r->buf.buf, end, &pos, &hop) < 0 ||
            moq_varint_read(r->buf.buf, end, &pos, &active) < 0 || pos != end) {
            LOG_WARN("[moq-lite-cli] malformed ANNOUNCE_OK");
            quic_client_stream_reset(r->s, kStreamInternalError);
            return;
        }
        r->ann_ok_done = 1;
        r->ann_expect = active;
        buf_consume(&r->buf, end);
        LOG_INFO("[moq-lite-cli] ANNOUNCE_OK: hop=%llu active=%llu prefix=\"%s\"",
                 (unsigned long long)hop, (unsigned long long)active,
                 prefix[0] ? prefix : "(all)");
        if (active == 0)
            LOG_WARN("[moq-lite-cli] announce returned an EMPTY set for "
                     "prefix=\"%s\" — either this prefix holds nothing, or the "
                     "prefix is wrong (both look identical otherwise)",
                     prefix[0] ? prefix : "(all)");
    }

    for (;;) {
        size_t toff = 0;
        uint64_t type = 0;
        if (moq_varint_read(r->buf.buf, r->buf.len, &toff, &type) < 0) break;
        size_t boff = toff, blen = 0;
        MoqMsgStatus st = moq_msg_read(r->buf.buf, r->buf.len, &boff, &blen);
        if (st == MOQ_MSG_TOO_LARGE) {
            quic_client_stream_reset(r->s, kStreamInternalError);
            return;
        }
        if (st == MOQ_MSG_NEED_MORE) break;

        const size_t end = boff + blen;
        const uint8_t *body = r->buf.buf + boff;

        if (type == kAnnStart) {
            size_t pos = 0;
            MoqStr suffix;
            uint64_t hop_count = 0, warm = 0, cold = 0, v = 0;
            /* ANNOUNCE_START{ suffix(s); hop count(i); hop ids…; warm(i); cold(i) }
             * ⚠️ 代价顺序 **warm 在前、cold 在后**（参考实现的测试特意用不对称的
             * (12,30) 防这个写反）。 */
            int ok = moq_str_read(body, blen, &pos, &suffix) == 0 &&
                     moq_varint_read(body, blen, &pos, &hop_count) == 0 &&
                     hop_count <= MOQ_MAX_HOPS;
            for (uint64_t i = 0; ok && i < hop_count; i++)
                ok = moq_varint_read(body, blen, &pos, &v) == 0;
            ok = ok && moq_varint_read(body, blen, &pos, &warm) == 0 &&
                 moq_varint_read(body, blen, &pos, &cold) == 0 && pos == blen;
            if (!ok) {
                LOG_WARN("[moq-lite-cli] malformed ANNOUNCE_START");
                quic_client_stream_reset(r->s, kStreamInternalError);
                return;
            }
            const uint64_t id = r->ann_next_id++;
            if (r->ann_expect > 0) r->ann_expect--;

            char *sfx = moq_str_dup(&suffix);
            LOG_INFO("[moq-lite-cli] ANNOUNCE_START id=%llu prefix=\"%s\" "
                     "suffix=\"%s\" hops=%llu cost(warm=%llu cold=%llu)",
                     (unsigned long long)id, prefix[0] ? prefix : "(all)",
                     sfx ? sfx : "", (unsigned long long)hop_count,
                     (unsigned long long)warm, (unsigned long long)cold);

            MoqBc *bc = (MoqBc*)calloc(1, sizeof(*bc));
            if (bc) {
                bc->used    = 1;
                bc->sess    = s;
                bc->prefix  = strdup(prefix);
                bc->suffix  = strdup(sfx ? sfx : "");
                bc->path    = join_path(prefix, sfx ? sfx : "");
                bc->ann_id  = id;
                size_t slot = alloc_slot((void***)&g_ctx.bcs, &g_ctx.bc_cnt,
                                         &g_ctx.bc_cap, bc, sizeof(MoqBc*));
                if (slot == (size_t)-1 || !bc->prefix || !bc->suffix || !bc->path) {
                    free(bc->prefix); free(bc->suffix); free(bc->path);
                    free(bc);
                } else {
                    bc->slot = (uint32_t)slot;
                    alloc_slot((void***)&s->bcs, &s->bc_cnt, &s->bc_cap, bc,
                               sizeof(MoqBc*));
                    if (s->cb.on_announce)
                        s->cb.on_announce(s->slot, bc->slot, bc->path,
                                          sfx ? sfx : "", s->cb.user);
                }
            }
            free(sfx);
        } else if (type == kAnnEnd) {
            size_t pos = 0;
            uint64_t id = 0;
            if (moq_varint_read(body, blen, &pos, &id) < 0 || pos != blen) {
                quic_client_stream_reset(r->s, kStreamInternalError);
                return;
            }
            /* ann_id 是这条流上的**隐式**序号（线上不带，两端各自数）。 */
            for (size_t i = 0; i < s->bc_cnt; i++) {
                MoqBc *bc = s->bcs[i];
                if (bc && !bc->dead && bc->ann_id == id) {
                    bc->dead = 1;
                    LOG_INFO("[moq-lite-cli] ANNOUNCE_END id=%llu path=\"%s\"",
                             (unsigned long long)id, bc->path);
                    if (s->cb.on_announce_end)
                        s->cb.on_announce_end(s->slot, bc->slot, s->cb.user);
                }
            }
        } else {
            /* 未知类型按长度跳过，不打断流 —— 旧实现对不了新消息，但不该因为
             * 这个把整条流带走。 */
            LOG_DEBUG("[moq-lite-cli] announce: unknown message type %llu "
                      "(%zu bytes) skipped", (unsigned long long)type, blen);
        }

        buf_consume(&r->buf, end);
    }

    if (r->fin && !r->done) r->done = 1;
}

/* Subscribe 应答（bidi 0x2，应答在**同一条**流上）：
 *   0x0 START{ group(i) }  接受 —— -06 上**没有** SUBSCRIBE_OK，这就是接受
 *   0x1 END  { group(i) }  **排他**上界，track 结束
 *   0x2 DROP { gs(i); ge(i, **含**); ec(i) }
 * 拒绝**不是**消息，是对端 reset 这条流（见 on_session_stream_reset）。 */
static void parse_subscribe(MoqStream *r) {
    MoqSub *sub = r->sub;

    for (;;) {
        size_t toff = 0;
        uint64_t type = 0;
        if (moq_varint_read(r->buf.buf, r->buf.len, &toff, &type) < 0) break;
        size_t boff = toff, blen = 0;
        MoqMsgStatus st = moq_msg_read(r->buf.buf, r->buf.len, &boff, &blen);
        if (st == MOQ_MSG_TOO_LARGE) {
            quic_client_stream_reset(r->s, kStreamInternalError);
            return;
        }
        if (st == MOQ_MSG_NEED_MORE) break;

        const size_t end = boff + blen;
        const uint8_t *body = r->buf.buf + boff;
        size_t pos = 0;
        uint64_t a = 0, b = 0, c = 0;
        int ok = 1;

        if (type == kRespStart) {
            ok = moq_varint_read(body, blen, &pos, &a) == 0 && pos == blen;
            if (ok && sub) {
                sub->started = 1;
                LOG_INFO("[moq-lite-cli] sub %u: SUBSCRIBE_START first_group=%llu",
                         sub->sub_id, (unsigned long long)a);
                if (sub->cb.on_subscribe)
                    sub->cb.on_subscribe(sub->slot, a, sub->cb.user);
            }
        } else if (type == kRespEnd) {
            /* -06 上**没有** Streams 字段（那是 07 专有，多读一个 varint 就是错）。*/
            ok = moq_varint_read(body, blen, &pos, &a) == 0 && pos == blen;
            if (ok && sub) {
                LOG_INFO("[moq-lite-cli] sub %u: SUBSCRIBE_END end_group=%llu "
                         "(exclusive — track done)", sub->sub_id,
                         (unsigned long long)a);
                if (sub->cb.on_subscribe_end)
                    sub->cb.on_subscribe_end(sub->slot, a, sub->cb.user);
            }
        } else if (type == kRespDrop) {
            ok = moq_varint_read(body, blen, &pos, &a) == 0 &&
                 moq_varint_read(body, blen, &pos, &b) == 0 &&
                 moq_varint_read(body, blen, &pos, &c) == 0 && pos == blen;
            if (ok && sub) {
                LOG_INFO("[moq-lite-cli] sub %u: SUBSCRIBE_DROP groups %llu..%llu "
                         "(inclusive) error=%llu", sub->sub_id,
                         (unsigned long long)a, (unsigned long long)b,
                         (unsigned long long)c);
                if (sub->cb.on_drop)
                    sub->cb.on_drop(sub->slot, a, b, c, sub->cb.user);
            }
        } else {
            LOG_DEBUG("[moq-lite-cli] subscribe: unknown response type %llu skipped",
                      (unsigned long long)type);
        }

        buf_consume(&r->buf, end);
        if (!ok) {
            LOG_WARN("[moq-lite-cli] malformed subscribe response");
            quic_client_stream_reset(r->s, kStreamInternalError);
            return;
        }
    }

    if (r->fin && !r->done) r->done = 1;
}

/* TRACK_INFO（bidi 0x6，本端开的）：唯一应答，随后对端 FIN。
 *   [len][priority(u8)][max_age(i)][timescale(i)]
 * ⚠️ **没有 group-order 字节**（01~05 专有）—— 照 Lite05 的样例抄会多一个字节，
 * 参考实现自带的跨语言断言里那个 [06 00 00 53 88 43 E8] 就是 Lite05 的。 */
static void parse_track(MoqStream *r) {
    MoqSub *sub = r->sub;

    size_t off = 0, blen = 0;
    MoqMsgStatus st = moq_msg_read(r->buf.buf, r->buf.len, &off, &blen);
    if (st == MOQ_MSG_TOO_LARGE) {
        LOG_WARN("[moq-lite-cli] TRACK_INFO over max message size");
        if (sub && sub->phase == kPhaseTrackReq) {
            sub->phase = kPhaseSubReq;
            sub_open_subscribe(sub);
        }
        return;
    }
    if (st == MOQ_MSG_NEED_MORE) {
        if (r->fin && sub && sub->phase == kPhaseTrackReq) {
            /* 对端没给 TRACK_INFO 就 FIN 了。**不能卡死**：直接进订阅阶段 ——
             * timescale 我们本来也不用来解释数据（内层微秒前缀是自描述的）。 */
            LOG_WARN("[moq-lite-cli] sub %u: track stream ended without TRACK_INFO; "
                     "proceeding to subscribe", sub->sub_id);
            r->done = 1;
            sub->phase = kPhaseSubReq;
            sub_open_subscribe(sub);
        }
        return;
    }

    const size_t end = off + blen;
    size_t pos = off;
    uint64_t max_age = 0, timescale = 0;
    uint8_t priority = 0;
    if (pos < end) priority = r->buf.buf[pos++];
    if (moq_varint_read(r->buf.buf, end, &pos, &max_age) < 0 ||
        moq_varint_read(r->buf.buf, end, &pos, &timescale) < 0 || pos != end) {
        LOG_WARN("[moq-lite-cli] malformed TRACK_INFO");
    } else if (sub) {
        LOG_INFO("[moq-lite-cli] sub %u: TRACK_INFO priority=%u max_age=%llums "
                 "timescale=%llu", sub->sub_id, (unsigned)priority,
                 (unsigned long long)max_age, (unsigned long long)timescale);
        if (sub->cb.on_track_info)
            sub->cb.on_track_info(sub->slot, priority, max_age, timescale,
                                  sub->cb.user);
    }
    buf_consume(&r->buf, end);

    if (sub && sub->phase == kPhaseTrackReq) {
        sub->phase = kPhaseSubReq;
        sub_open_subscribe(sub);
    }
    if (r->fin) r->done = 1;
}

/* 解析入口：每次数据到达都会调，必须能从中间状态继续。 */
/* ============================================================
 * 发布端（serve）—— 对端来订阅我们
 *
 * 三条流都遵循同一套骨架：对端开一条 bidi 请求流过来，我们在**同一条流上**回。
 * 所以流 id 本身就是关联键，不需要另做 ID 配对。
 *
 * ⚠️ 回复必须等**开流回调**拿到 quic_stream_t 才能写 —— 对端开的流我们这边
 * 第一次见到它是在 on_stream_data 里，而那条流本来就已经可写。但**我们主动
 * 开的**流（组流）是异步的：quic_client_open_uni_stream 之后才在回调里拿到
 * 句柄。所以组流的头（STREAM_TYPE + GROUP 头）是在回调里一次写出的。
 * ============================================================ */

static const char *kCatalogTrack = "catalog.json";

/* 发布端常量。timescale **必须** 1000000：payload 开头那个容器时间戳前缀恒为
 * 微秒，而外层 FRAME 差值的单位就是我们在 TRACK_INFO 里声明的 track timescale
 * —— 两个尺度必须一致，否则对端按另一个尺度换算差值，表现是播放速度离谱。 */
#define MOQ_SERVE_TIMESCALE   1000000
#define MOQ_SERVE_MAX_AGE_MS  5000
#define MOQ_SERVE_PRIORITY    0
/* 音频按 1 秒一组（与服务端 kAudioGroupUs 同值）。 */
#define MOQ_SERVE_AUDIO_GROUP_US (1000 * 1000)

/* 按轨名在我们的发布表里找一条轨（serve 侧）。 */
static MoqPubTrack *pub_find_track(MoqSession *s, const char *path,
                                   const char *name) {
    if (!path || !name) return NULL;
    for (size_t i = 0; i < s->pub_cnt; i++) {
        MoqPub *p = s->pubs[i];
        if (!p || !p->used || p->dead) continue;
        if (strcmp(p->path, path) != 0) continue;
        for (int k = 0; k < p->track_cnt; k++) {
            MoqPubTrack *t = p->tracks[k];
            if (t && !t->dead && strcmp(t->name, name) == 0) return t;
        }
    }
    return NULL;
}

/* 我们对外公告的广播里，有没有 path 在这个 prefix scope 内的。
 * 有 → 写回 out（相对请求前缀的 suffix）；没有 → 0。
 *
 * 与服务端 AnnounceGrid 的 rebase 规则一致：
 *   prefix 空          → suffix = 全名
 *   全名 == prefix     → suffix 空（合法且最常见）
 *   全名前缀匹配 + '/' → suffix = 去掉 "prefix/" 之后
 *   否则               → 不在这个 scope 里 */
static int pub_suffix_for(MoqSession *s, const char *prefix,
                          const char *wire, char **out) {
    const char *a = prefix ? prefix : "";
    *out = NULL;
    if (!*a) { *out = strdup(wire); return *out != NULL; }
    if (strcmp(wire, a) == 0) { *out = strdup(""); return *out != NULL; }
    size_t la = strlen(a);
    if (strlen(wire) > la && strncmp(wire, a, la) == 0 && wire[la] == '/') {
        *out = strdup(wire + la + 1);
        return *out != NULL;
    }
    (void)s;
    return 0;
}

/* 应答对端的 ANNOUNCE_REQUEST：ANNOUNCE_OK + N×ANNOUNCE_START，**一次性写出**，
 * fin=0（这条流要一直开着，后面的更新还走它）。 */
static int serve_announce_write(MoqSession *s, quic_stream_t *st,
                                const char *prefix) {
    MoqBuf out; moq_buf_init(&out);

    /* 先数一遍有几条要公告 —— ANNOUNCE_OK 的 active 得是真实条数。 */
    MoqBuf starts; moq_buf_init(&starts);
    uint64_t active = 0;
    for (size_t i = 0; i < s->pub_cnt; i++) {
        MoqPub *p = s->pubs[i];
        if (!p || !p->used || p->dead) continue;
        if (p->track_cnt == 0) continue;   /* 没有轨 = 没有 catalog，不公告 */
        char *suffix = NULL;
        if (!pub_suffix_for(s, prefix, p->path, &suffix)) continue;
        if (moq_wire_announce_start(&starts, suffix, 0, 0) == 0) active++;
        LOG_INFO("[moq-lite-cli] announcing broadcast \"%s\" (prefix=\"%s\" "
                 "suffix=\"%s\")", p->path, prefix[0] ? prefix : "(all)", suffix);
        free(suffix);
    }

    int rc = moq_wire_announce_ok(&out, 0, active);
    if (rc == 0) rc = moq_buf_append(&out, starts.buf, starts.len);
    moq_buf_free(&starts);

    if (rc != 0) {
        LOG_ERROR("[moq-lite-cli] cannot encode announce reply");
        moq_buf_free(&out);
        return 0;
    }
    quic_client_stream_write(st, out.buf, out.len, 0, NULL, NULL, 0);
    LOG_INFO("[moq-lite-cli] ANNOUNCE_OK sent: active=%llu prefix=\"%s\"",
             (unsigned long long)active, prefix[0] ? prefix : "(all)");
    moq_buf_free(&out);
    return 1;
}

/* 发布侧的 Track 请求应答：找到轨就发 TRACK_INFO 并 FIN；找不到 reset。
 * ps->reply 里已经存好了编码结果（在解析时算的），这里只负责写。 */
static void serve_replay_hold(MoqServeSub *ss, MoqPubTrack *t);

static void serve_track_info_write(MoqPeerStream *ps, quic_stream_t *st) {
    if (ps->reply.len == 0) {
        /* 没编码成功 = 轨不存在（或 alloc 失败）—— 拒绝订阅是 reset 那条流，
         * 不是发一条消息（-06 没有 TRACK_INFO_ERROR 这种东西）。 */
        LOG_INFO("[moq-lite-cli] TRACK request for an unknown track — resetting");
        quic_client_stream_reset(st, kStreamCancelled);
    } else {
        /* fin=1：发布端写完 TRACK_INFO 立刻 FIN 这条流。不 FIN 的话对端会一直
         * 等更多属性（参考实现 poll_flush + finish 就是这么做的）。 */
        quic_client_stream_write(st, ps->reply.buf, ps->reply.len, 1,
                                 NULL, NULL, 0);
        LOG_INFO("[moq-lite-cli] TRACK_INFO sent (fin) on reply stream");
    }
    moq_buf_free(&ps->reply);
    ps->reply_sent = 1;
}

/* ── 组的开出与关闭 ─────────────────────────── */


/* 组流就绪（uni 开流回调）：STREAM_TYPE(0x0) + GROUP 头一次写出。
 * 组号按**订阅**编（ss->group_seq），不是按轨。 */
/* ── 组帧缓冲 ─────────────────────────────────
 *
 * ⚠️ 这一层不是为了「少丢几帧」—— 第一帧丢不得：视频那边开组的那一帧**就是
 * 关键帧**，丢了整组都解不出来。实测症状：视频完全出不来。 */
/* 攒下的帧数上限。开流窗口是毫秒级，正常只有 1~2 帧；超出说明真卡住了，
 * 丢掉并告警，别让内存无界涨。 */
#define MOQ_MAX_PEND_FRAMES 64

static void serve_frame_drop(MoqServeSub *ss) {
    for (size_t i = 0; i < ss->pend_cnt; i++) free(ss->pend[i].data);
    ss->pend_cnt = 0;
}

/* 把一帧真正写进**当前已开**的组流（调用方保证 ss->uni != NULL）。 */
static void serve_write_one(MoqServeSub *ss, uint64_t ts_us,
                            const uint8_t *payload, size_t len) {
    /* 帧体 = [绝对微秒 varint][裸编解码字节] —— Legacy 容器的第二层。
     * 外层 FRAME 的差值是**组内累加**的（group_prev_us 每组清零）。 */
    MoqBuf body; moq_buf_init(&body);
    MoqBuf seg; moq_buf_init(&seg);
    int ok = moq_varint_write(&body, ts_us) == 0 &&
             moq_buf_append(&body, payload, len) == 0;
    if (ok) {
        int64_t delta = (int64_t)ts_us - ss->group_prev_us;
        ok = moq_wire_frame(&seg, delta, body.buf, body.len) == 0;
    }
    if (ok) quic_client_stream_write(ss->uni, seg.buf, seg.len, 0,
                                     NULL, NULL, 0);
    ss->group_prev_us = (int64_t)ts_us;
    ss->group_had_media = 1;
    ss->frames++;
    moq_buf_free(&body);
    moq_buf_free(&seg);
}

static int serve_frame_buffer(MoqServeSub *ss, uint64_t ts_us,
                              const uint8_t *payload, size_t len) {
    if (ss->pend_cnt >= MOQ_MAX_PEND_FRAMES) return -1;
    if (ss->pend_cnt == ss->pend_cap) {
        size_t want = ss->pend_cap ? ss->pend_cap * 2 : 8;
        ServeFrame *nb = (ServeFrame*)realloc(ss->pend, want * sizeof(*nb));
        if (!nb) return -1;
        ss->pend = nb;
        ss->pend_cap = want;
    }
    uint8_t *copy = (uint8_t*)malloc(len ? len : 1);
    if (!copy) return -1;
    if (len) memcpy(copy, payload, len);
    ss->pend[ss->pend_cnt].ts_us = ts_us;
    ss->pend[ss->pend_cnt].data  = copy;
    ss->pend[ss->pend_cnt].len   = len;
    ss->pend_cnt++;
    return 0;
}

static void serve_frame_flush(MoqServeSub *ss) {
    for (size_t i = 0; i < ss->pend_cnt; i++) {
        if (ss->uni)
            serve_write_one(ss, ss->pend[i].ts_us, ss->pend[i].data,
                            ss->pend[i].len);
        free(ss->pend[i].data);
    }
    ss->pend_cnt = 0;
}

static void serve_group_open_on_stream(MoqServeSub *ss, quic_stream_t *st) {
    ss->uni = st;
    if (!ss->started) {
        /* 开组流 = 我们已经准备好了，逐帧数据马上要来。
         * SUBSCRIBE_START **不能早发** —— 里面的 group 号必须是诚实的（对端会
         * 拿它当基线），所以推迟到「第一个组真要写出去」的这一刻。 */
        MoqBuf ack; moq_buf_init(&ack);
        if (moq_wire_subscribe_start(&ack, 0) == 0 && ss->sub_stream) {
            quic_client_stream_write(ss->sub_stream, ack.buf, ack.len, 0,
                                     NULL, NULL, 0);
            ss->started = 1;
            LOG_INFO("[moq-lite-cli] sub %llu: SUBSCRIBE_START sent (accept is "
                     "implicit on -06; no SUBSCRIBE_OK)",
                     (unsigned long long)ss->sid);
        }
        moq_buf_free(&ack);
    }

    MoqBuf out; moq_buf_init(&out);
    /* ⚠️ 第三个字段是 **frame start（组内首帧的下标）**，恒为 0 —— 不是组的
     * 开组时间戳。这里曾经错传了 group_open_us，后果是**静默且致命**：
     * 收侧用 `key_pending = (frame_start == 0)` 判关键帧（Legacy 线上没有
     * 关键帧标志位，只能靠「组内第一帧」这个不变量），frame_start 非 0 就意味着
     * **这一组里一帧都不会被当成关键帧**；服务端的 egress 只在关键帧上开组，
     * 于是**整条视频全被丢掉**（实测：ingest 侧 delivered 一直在涨，egress 侧
     * frames=0，浏览器/拉流端一点画面都没有）。 */
    if (moq_put_stream_type(&out, 0x0) < 0 ||
        moq_wire_group_header(&out, ss->sid, ss->group_seq, 0) < 0) {
        LOG_ERROR("[moq-lite-cli] cannot encode group header");
        quic_client_stream_reset(st, kStreamInternalError);
        ss->uni = NULL;
        return;
    }
    quic_client_stream_write(st, out.buf, out.len, 0, NULL, NULL, 0);
    moq_buf_free(&out);

    ss->group_opening = 0;
    /* 组头已经在线上了 —— 补上开组期间攒下的帧（第一帧就是开组那一帧，
     * 视频那边是关键帧，丢了整组解不出来）。 */
    serve_frame_flush(ss);
}

/* catalog.json 的下行：**不走组写入器**。一组一帧、载荷就是裸 JSON（既没有
 * 容器微秒前缀，外层帧的差也是 0），发完即 FIN。
 *
 * ⚠️ 组号恒 0：这条订阅的 catalog 只有这一组。将来 catalog 变了要发**下一组**
 * （seq=1），不是重发组 0 —— 对端按组号去重，重发会被丢掉。 */
static void serve_catalog_on_stream(MoqServeSub *ss, quic_stream_t *st) {
    if (!ss->catalog_json || ss->catalog_len == 0) {
        quic_client_stream_reset(st, kStreamInternalError);
        return;
    }
    MoqBuf out; moq_buf_init(&out);
    int ok = moq_put_stream_type(&out, 0x0) == 0 &&
             moq_wire_group_header(&out, ss->sid, 0, 0) == 0 &&
             moq_wire_frame(&out, 0, ss->catalog_json, ss->catalog_len) == 0;
    if (ok) {
        quic_client_stream_write(st, out.buf, out.len, 1, NULL, NULL, 0);
        LOG_INFO("[moq-lite-cli] catalog.json sent: %zu bytes, one group, FIN",
                 ss->catalog_len);
    } else {
        quic_client_stream_reset(st, kStreamInternalError);
    }
    moq_buf_free(&out);
    ss->dead = 1;   /* catalog 一次性的，之后这个订阅记录不再有用 */
}

/* 关掉当前组：写一个**结束哨兵**并让 FIN 搭在它上面。
 *
 * ⚠️ moq-lite 里**一个组 = 一条 uni 流**，所以组结束**就是**流结束 —— 每次
 * 关组都要 FIN。少了 FIN，对端只能靠时间预算猜组边界（组的推进本来就是靠
 * 「读到流末尾」），实测症状是音频只出一小段就没声了。
 *
 * ⚠️ 哨兵的真实形状是「只有 varint 时间戳前缀、0 字节编解码载荷」，不是空帧
 * —— 参考实现 fn end() = frame.payload.is_empty().then_some(ts)，即解出时间戳
 * **之后**载荷为空才算。
 * ⚠️ FIN **必须**搭在一个非空段上：本仓 quic_stream_write 对 len==0+fin=1 的
 * 请求会在 send_buf 已排空时**静默丢弃 FIN 且返回 0**（见
 * [quic-zero-length-fin-dropped]）。哨兵帧正好提供了那个非空段。 */
static void serve_group_close(MoqServeSub *ss) {
    if (!ss->uni) {
        /* 开流还在路上就被关了（连着两个关键帧之间一帧都没写进去）。
         * 攒下的帧属于这个没开成的组 —— 丢掉，别漏进下一个组。 */
        if (ss->pend_cnt) serve_frame_drop(ss);
        ss->group_opening = 0;
        return;
    }
    const int64_t end_us = ss->group_had_media ? ss->group_prev_us
                                               : ss->group_open_us;
    MoqBuf payload; moq_buf_init(&payload);
    MoqBuf seg; moq_buf_init(&seg);
    if (moq_varint_write(&payload, (uint64_t)end_us) == 0 &&
        moq_wire_frame(&seg, 0, payload.buf, payload.len) == 0) {
        quic_client_stream_write(ss->uni, seg.buf, seg.len, 1, NULL, NULL, 0);
    }
    moq_buf_free(&payload);
    moq_buf_free(&seg);
    ss->uni = NULL;
    ss->group_had_media = 0;
    ss->group_seq++;
}


/* 把一帧写进当前组流；没有组流就先发起开组，并把这一帧攒下来等组头。 */
static void serve_write_frame(MoqServeSub *ss, uint64_t ts_us,
                              const uint8_t *payload, size_t len) {
    if (ss->dead || !ss->sub_stream) return;

    if (!ss->uni) {
        if (!ss->group_opening) {
            ss->group_opening = 1;
            ss->group_open_us = (int64_t)ts_us;
            ss->group_prev_us = 0;   /* 组内累加基线，每组重置 */
            ss->group_had_media = 0;
            pending_push_box(kOpenTrackGroup, ss);
            quic_client_open_uni_stream(ss->sess->cli);
        }
        /* ⚠️ 这一帧是组的**第一帧**（视频那边就是关键帧），必须攒下来。
         * 早先的写法在这里直接 return 把它丢了 —— 组里没有关键帧，对端永远
         * 解不出来，症状是「视频完全不出来」。 */
        if (serve_frame_buffer(ss, ts_us, payload, len) < 0)
            LOG_WARN("[moq-lite-cli] sub %llu: cannot buffer the group's first "
                     "frame", (unsigned long long)ss->sid);
        return;
    }
    serve_write_one(ss, ts_us, payload, len);
}

/* ── 请求流的解析 ───────────────────────────── */

/* 把一条 [Type?][Length][body] 切出来；返回 1 = 拿到，0 = 不够。 */

/* 对端开的 ANNOUNCE 流：唯一的请求是 [len][prefix(s)]。 */
static void serve_parse_announce(MoqStream *r) {
    size_t off = 0, blen = 0;
    MoqMsgStatus st = moq_msg_read(r->buf.buf, r->buf.len, &off, &blen);
    if (st == MOQ_MSG_NEED_MORE) return;
    if (st == MOQ_MSG_TOO_LARGE) {
        quic_client_stream_reset(r->s, kStreamInternalError);
        r->done = 1;
        return;
    }
    size_t pos = off;
    MoqStr prefix;
    if (moq_str_read(r->buf.buf, off + blen, &pos, &prefix) != 0 || pos != off + blen) {
        LOG_WARN("[moq-lite-cli] malformed ANNOUNCE_REQUEST");
        quic_client_stream_reset(r->s, kStreamInternalError);
        r->done = 1;
        return;
    }
    char *pfx = moq_str_dup(&prefix);
    buf_consume(&r->buf, off + blen);
    LOG_INFO("[moq-lite-cli] peer ANNOUNCE_REQUEST: prefix=\"%s\"",
             pfx && pfx[0] ? pfx : "(all)");

    /* ⚠️ 应答写在**同一条**流上 —— 这条流是对端开的，我们这边已经可写，
     * 不需要再开流。 */
    if (!serve_announce_write(r->sess, r->s, pfx ? pfx : ""))
        quic_client_stream_reset(r->s, kStreamInternalError);
    free(pfx);
    /* 不置 done：这条流要一直开着（后续更新还走它）。 */
}

/* 对端开的 TRACK 流：请求 [len][broadcast(s)][track(s)]，应答 TRACK_INFO + FIN。*/
static void serve_parse_track(MoqStream *r) {
    size_t off = 0, blen = 0;
    MoqMsgStatus st = moq_msg_read(r->buf.buf, r->buf.len, &off, &blen);
    if (st == MOQ_MSG_NEED_MORE) {
        if (r->fin) r->done = 1;
        return;
    }
    if (st == MOQ_MSG_TOO_LARGE) {
        quic_client_stream_reset(r->s, kStreamInternalError);
        r->done = 1;
        return;
    }
    size_t pos = off;
    MoqStr bc, tr;
    if (moq_str_read(r->buf.buf, off + blen, &pos, &bc) != 0 ||
        moq_str_read(r->buf.buf, off + blen, &pos, &tr) != 0 || pos != off + blen) {
        LOG_WARN("[moq-lite-cli] malformed TRACK request");
        quic_client_stream_reset(r->s, kStreamInternalError);
        r->done = 1;
        return;
    }
    char *broadcast = moq_str_dup(&bc);
    char *track = moq_str_dup(&tr);
    buf_consume(&r->buf, off + blen);
    r->done = 1;   /* Track 流是一次性的：答完就结束 */

    MoqPubTrack *t = pub_find_track(r->sess, broadcast, track);
    LOG_INFO("[moq-lite-cli] peer TRACK request: broadcast=\"%s\" track=\"%s\" -> %s",
             broadcast ? broadcast : "", track ? track : "",
             t ? "found" : "unknown (will reset)");

    MoqPeerStream *ps = (MoqPeerStream*)calloc(1, sizeof(*ps));
    if (ps) {
        ps->used = 1;
        ps->in = r->s;
        ps->sess = r->sess;
        ps->wire_type = kBidiTrack;
        moq_buf_init(&ps->reply);
        if (t &&
            moq_wire_track_info(&ps->reply, MOQ_SERVE_PRIORITY,
                                MOQ_SERVE_MAX_AGE_MS,
                                MOQ_SERVE_TIMESCALE) != 0) {
            moq_buf_free(&ps->reply);
        }
        uint64_t slot = alloc_slot((void***)&r->sess->peers, &r->sess->peer_cnt,
                                   &r->sess->peer_cap, ps, sizeof(MoqPeerStream*));
        if (slot == (size_t)-1) {
            moq_buf_free(&ps->reply);
            free(ps);
            quic_client_stream_reset(r->s, kStreamInternalError);
        } else {
            ps->slot = (uint32_t)slot;
            /* 我们已经握着对端开的流句柄（它可写），所以直接写，不必等开流。*/
            serve_track_info_write(ps, r->s);
            /* 这条流一次性，写完之后记录本身可以放掉 —— 但 slot 还挂着，
             * 标 dead 让它失效即可。 */
            ps->dead = 1;
        }
    }
    free(broadcast);
    free(track);
}

/* 对端开的 SUBSCRIBE 流：请求字段顺序
 *   id(i) broadcast(s) track(s) priority(u8) max_age(i)
 *   start_group(i) end_group(i) start_frame(i) end_frame(i)
 * ⚠️ 三处易错：① -06 **没有** group-order 字节；② priority 是**裸 u8**；
 * ③ start_* 的 0 = 不限，end_* 是 N+1 形式、0 也 = 不限。 */
static void serve_parse_subscribe(MoqStream *r) {
    size_t off = 0, blen = 0;
    MoqMsgStatus st = moq_msg_read(r->buf.buf, r->buf.len, &off, &blen);
    if (st == MOQ_MSG_NEED_MORE) {
        if (r->fin) r->done = 1;
        return;
    }
    if (st == MOQ_MSG_TOO_LARGE) {
        quic_client_stream_reset(r->s, kStreamInternalError);
        r->done = 1;
        return;
    }
    const size_t end = off + blen;
    const uint8_t *p = r->buf.buf;
    size_t pos = off;
    uint64_t sub_id = 0, priority = 0, max_age = 0;
    uint64_t sg = 0, eg = 0, sf = 0, ef = 0;
    MoqStr bc, tr;
    int ok = moq_varint_read(p, end, &pos, &sub_id) == 0 &&
             moq_str_read(p, end, &pos, &bc) == 0 &&
             moq_str_read(p, end, &pos, &tr) == 0 &&
             pos < end;              /* 这条检查不能省：后面要读裸 u8 */
    if (ok) priority = p[pos++];
    ok = ok && moq_varint_read(p, end, &pos, &max_age) == 0 &&
         moq_varint_read(p, end, &pos, &sg) == 0 &&
         moq_varint_read(p, end, &pos, &eg) == 0 &&
         moq_varint_read(p, end, &pos, &sf) == 0 &&
         moq_varint_read(p, end, &pos, &ef) == 0 && pos == end;
    if (!ok) {
        LOG_WARN("[moq-lite-cli] malformed SUBSCRIBE");
        quic_client_stream_reset(r->s, kStreamInternalError);
        r->done = 1;
        return;
    }
    char *broadcast = moq_str_dup(&bc);
    char *track = moq_str_dup(&tr);
    buf_consume(&r->buf, end);
    r->done = 1;

    MoqPubTrack *t = pub_find_track(r->sess, broadcast, track);
    int is_catalog = (track && strcmp(track, kCatalogTrack) == 0) &&
                     broadcast && pub_find_track(r->sess, broadcast, kCatalogTrack) == NULL
                     /* catalog 是合成的，不在 tracks 里 —— 用广播存在性判 */;
    LOG_INFO("[moq-lite-cli] peer SUBSCRIBE id=%llu broadcast=\"%s\" track=\"%s\" "
             "prio=%llu max_age=%llums", (unsigned long long)sub_id,
             broadcast ? broadcast : "", track ? track : "",
             (unsigned long long)priority, (unsigned long long)max_age);

    /* catalog.json：合成的一轨，单独发（不是媒体，不走组写入器）。
     * 找不到对应广播就 reset。 */
    if (is_catalog) {
        MoqPub *pub = NULL;
        for (size_t i = 0; i < r->sess->pub_cnt; i++) {
            MoqPub *q = r->sess->pubs[i];
            if (q && q->used && !q->dead && strcmp(q->path, broadcast) == 0) { pub = q; break; }
        }
        if (!pub || pub->catalog.len == 0) {
            LOG_INFO("[moq-lite-cli] catalog subscribe rejected (no catalog)");
            quic_client_stream_reset(r->s, kStreamCancelled);
        } else {
            /* SUBSCRIBE_START + 一条 uni 组流（一帧裸 JSON）+ FIN。 */
            MoqBuf ack; moq_buf_init(&ack);
            if (moq_wire_subscribe_start(&ack, 0) == 0)
                quic_client_stream_write(r->s, ack.buf, ack.len, 0, NULL, NULL, 0);
            moq_buf_free(&ack);
            MoqServeSub *ss = (MoqServeSub*)calloc(1, sizeof(*ss));
            if (ss) {
                ss->used = 1; ss->sess = r->sess; ss->sid = sub_id;
                ss->sub_stream = r->s;
                uint64_t slot = alloc_slot((void***)&r->sess->serve_subs,
                                           &r->sess->serve_cnt, &r->sess->serve_cap,
                                           ss, sizeof(MoqServeSub*));
                if (slot != (size_t)-1) {
                    ss->slot = (uint32_t)slot;
                    ss->catalog_json = pub->catalog.buf;
                    ss->catalog_len = pub->catalog.len;
                    /* ⚠️ 别忘了开那条组流 —— 只发 SUBSCRIBE_START 不开流的话，
                     * 对端收到「接受」却永远等不到 catalog 那一帧，于是也
                     * 不会去订阅任何媒体轨（实测就是这样停住的：订阅进来了、
                     * 之后一片安静）。 */
                    pending_push_box(kOpenTrackGroup, ss);
                    quic_client_open_uni_stream(r->sess->cli);
                } else { free(ss); }
            }
        }
        free(broadcast); free(track);
        return;
    }

    if (!t) {
        /* 拒绝订阅 = **直接 reset 那条流**，不是发一条消息。 */
        LOG_INFO("[moq-lite-cli] subscribe rejected: track \"%s\" not published",
                 track ? track : "");
        quic_client_stream_reset(r->s, kStreamCancelled);
        free(broadcast); free(track);
        return;
    }

    MoqServeSub *ss = (MoqServeSub*)calloc(1, sizeof(*ss));
    if (!ss) { free(broadcast); free(track); return; }
    ss->used = 1;
    ss->sess = r->sess;
    ss->sid = sub_id;
    ss->sub_stream = r->s;
    ss->track = t;
    ss->group_seq = 0;
    uint64_t slot = alloc_slot((void***)&r->sess->serve_subs, &r->sess->serve_cnt,
                               &r->sess->serve_cap, ss, sizeof(MoqServeSub*));
    if (slot == (size_t)-1) { free(ss); free(broadcast); free(track); return; }
    ss->slot = (uint32_t)slot;
    LOG_INFO("[moq-lite-cli] subscription accepted: id=%llu track=\"%s\" "
             "(SUBSCRIBE_START deferred until the first group)",
             (unsigned long long)sub_id, t->name);
    /* 立刻从保留缓冲起步 —— 推流早就开始了，对端不该干等到下一个关键帧。 */
    serve_replay_hold(ss, t);
    free(broadcast); free(track);
}

static void stream_parse(MoqStream *r) {
    MoqSession *s = r->sess;
    if (r->done || r->dead || s->dead) return;

    /* STREAM_TYPE 只在**对端开的**流上读 —— 本端开的 bidi 流，类型字节是我们
     * 自己写进去的，对端在同一条流上回的应答上来就是消息。少了这一步，
     * ANNOUNCE_OK / TRACK_INFO 的首字节会被当成 STREAM_TYPE 解掉。 */
    if (!r->typed) {
        size_t off = 0;
        uint64_t type = 0;
        if (moq_varint_read(r->buf.buf, r->buf.len, &off, &type) < 0) {
            if (r->fin) {
                LOG_WARN("[moq-lite-cli] stream ended inside STREAM_TYPE");
                r->done = 1;
            }
            return;
        }
        r->typed = 1;
        buf_consume(&r->buf, off);

        if (r->uni) {
            /* 单向：0x1 = Setup（对端自己的 setup，读完即弃），其余按 Group。 */
            r->kind = (type == kUniSetup) ? kStreamSetup : kStreamGroup;
        } else {
            switch (type) {
            case kBidiAnnounce:  r->kind = kStreamAnnounce;  break;
            case kBidiSubscribe: r->kind = kStreamSubscribe; break;
            case kBidiTrack:     r->kind = kStreamTrack;     break;
            default:
                /* 对端开的未知 bidi 流（Fetch/Probe/Goaway 等）：**必须 reset，
                 * 不能静默忽略** —— 静默会让对端一直等一条永不到来的应答，
                 * 而按协议它本来就该收到一条。 */
                LOG_INFO("[moq-lite-cli] unsupported bidi stream type %llu — "
                         "resetting", (unsigned long long)type);
                quic_client_stream_reset(r->s, kStreamCancelled);
                r->done = 1;
                return;
            }
        }
    }

    /* 对端自己的 setup 流：参数（Path 等）只有服务端方向有意义，读到就扔。 */
    if (r->kind == kStreamSetup) { r->done = 1; return; }

    /* ⚠️ bidi 的两套解析**方向相反**，靠 r->local 区分：
     *   local=1：流是我们开的 → 对端的**应答**（应答不带 STREAM_TYPE，
     *            所以 r->kind 是开流时就定好的）。
     *   local=0：流是对端开的 → 对端的**请求**，我们要在**同一条流上**应答。
     * 混了就会把请求当应答解 —— 而两者的字节形状完全不同，会解出一堆看似
     * 合法但没有意义的数字（不报错）。 */
    if (!r->local) {
        switch (r->kind) {
        case kStreamAnnounce:  serve_parse_announce(r);  break;
        case kStreamSubscribe: serve_parse_subscribe(r); break;
        case kStreamTrack:     serve_parse_track(r);     break;
        default: break;
        }
        return;
    }

    switch (r->kind) {
    case kStreamGroup:     parse_group(r);     break;
    case kStreamAnnounce:  parse_announce(r);  break;
    case kStreamSubscribe: parse_subscribe(r); break;
    case kStreamTrack:     parse_track(r);     break;
    default: break;
    }
}

/* ============================================================
 * quic_client 回调
 * ============================================================ */

/* 开流请求按**调用顺序**完成，但回调里只拿得到 quic_stream_t —— 所以用一个
 * 队列把「这次开流是给谁的」传过去。只有 bidi 开流走它；uni 开流只有 SETUP
 * 一个用途，单列一个回调，不必进队列。 */
typedef struct { OpenKind kind; MoqSub *sub; void *box; } PendingOpen;

static PendingOpen *g_pending;
static size_t g_pending_cnt, g_pending_cap;

static void pending_push3(OpenKind kind, MoqSub *sub, void *box) {
    /* 这是**值**数组（不是指针数组），所以不走 alloc_slot —— 那个按 void*
     * 元素大小增长。这里手工增长。 */
    if (ensure_slot((void***)&g_pending, &g_pending_cap, g_pending_cnt,
                    sizeof(PendingOpen)) < 0) {
        LOG_ERROR("[moq-lite-cli] cannot queue stream-open intent");
        return;
    }
    g_pending[g_pending_cnt].kind = kind;
    g_pending[g_pending_cnt].sub  = sub;
    g_pending[g_pending_cnt].box  = box;
    g_pending_cnt++;
}

static void pending_push(OpenKind kind, MoqSub *sub) {
    pending_push3(kind, sub, NULL);
}

static void pending_push_box(OpenKind kind, void *box) {
    pending_push3(kind, NULL, box);
}

static int pending_pop(PendingOpen *out) {
    if (g_pending_cnt == 0) return 0;
    *out = g_pending[0];
    memmove(g_pending, g_pending + 1, (g_pending_cnt - 1) * sizeof(PendingOpen));
    g_pending_cnt--;
    return 1;
}

static void on_session_connected(quic_client_t *c, int status, void *user) {
    MoqSession *s = (MoqSession*)user;
    if (!s || s->dead) return;
    s->connecting = 0;
    s->connected = (status == 0);

    if (status != 0) {
        LOG_ERROR("[moq-lite-cli] connect failed: status=%d", status);
        if (s->cb.on_connected) s->cb.on_connected(s->slot, status, s->cb.user);
        return;
    }

    const char *alpn = QuicConnectionGetAlpn(quic_client_conn(c));
    LOG_INFO("[moq-lite-cli] connected: alpn=\"%s\"",
             alpn && alpn[0] ? alpn : "(none)");
    /* ⚠️ 必须核对协商结果：两边都 offer h3 时协商会落到 h3，服务端于是按
     * WebTransport 处理、等一个永远不来的 CONNECT —— 症状是「连上了但没有任何
     * 下文」，极难查。这里发现不对就直接判失败，别让它悄悄往下走。 */
    if (!alpn || strncmp(alpn, "moq-lite-", 9) != 0) {
        LOG_ERROR("[moq-lite-cli] negotiated ALPN \"%s\" is not moq-lite: the peer "
                  "will treat this session as WebTransport and never speak moq",
                  alpn && alpn[0] ? alpn : "(none)");
        if (s->cb.on_connected) s->cb.on_connected(s->slot, -100, s->cb.user);
        return;
    }

    quic_client_open_uni_stream(s->cli);   /* SETUP 流 */
    if (s->cb.on_connected) s->cb.on_connected(s->slot, 0, s->cb.user);
}

/* 唯一一处开 uni 流的地方就是 SETUP。 */
/* uni 开流只有两个来源：SETUP（连接建立时的那一条）和**发布侧给某条订阅开的
 * 组流**。开流回调只拿得到 quic_stream_t，所以按调用顺序弹意图队列区分 ——
 * 与 bidi 同理（见 pending_push 的说明）。 */
static void on_session_uni_open(quic_client_t *c, quic_stream_t *st, void *user) {
    (void)c;
    MoqSession *s = (MoqSession*)user;
    if (!s || s->dead) return;

    PendingOpen po = {0};
    if (pending_pop(&po) && po.kind == kOpenTrackGroup) {
        MoqServeSub *ss = (MoqServeSub*)po.box;
        if (!ss || ss->dead) { quic_client_stream_reset(st, kStreamInternalError); return; }
        if (ss->catalog_json) serve_catalog_on_stream(ss, st);
        else                  serve_group_open_on_stream(ss, st);
        return;
    }
    /* 没排到意图 = 就是 SETUP 那条（它是连接建立后第一条 uni 开流，
     * 也是唯一不走队列的）。 */
    s->setup_stream = st;
    /* 注册成 done：对端不会在这条流上写任何东西，真写了也别当协议消息解。 */
    MoqStream *r = stream_add(s, st, kStreamSetup, 1);
    if (r) r->done = 1;
    session_send_setup(s);
}

static void on_session_bidi_open(quic_client_t *c, quic_stream_t *st, void *user) {
    (void)c;
    MoqSession *s = (MoqSession*)user;
    if (!s || s->dead) return;

    PendingOpen po = {0};
    if (!pending_pop(&po)) {
        LOG_WARN("[moq-lite-cli] unsolicited bidi stream open — resetting");
        quic_client_stream_reset(st, kStreamInternalError);
        return;
    }

    switch (po.kind) {
    case kOpenAnnounce:
        s->announce_stream = st;
        stream_add(s, st, kStreamAnnounce, 1);
        session_send_announce(s, "");
        break;
    case kOpenTrack: {
        MoqSub *sub = po.sub;
        if (!sub || sub->dead) { quic_client_stream_reset(st, kStreamInternalError); break; }
        sub->track_stream = st;
        MoqStream *r = stream_add(s, st, kStreamTrack, 1);
        if (r) r->sub = sub;
        sub_pump(sub);
        break;
    }
    case kOpenSubscribe: {
        MoqSub *sub = po.sub;
        if (!sub || sub->dead) { quic_client_stream_reset(st, kStreamInternalError); break; }
        sub->sub_stream = st;
        MoqStream *r = stream_add(s, st, kStreamSubscribe, 1);
        if (r) r->sub = sub;
        sub_pump(sub);
        break;
    }
    case kOpenTrackGroup: {
        /* 给某条订阅开组流（uni）。catalog 那条特殊：一组一帧、裸 JSON、发完
         * 即 FIN，不走组写入器。 */
        MoqServeSub *ss = (MoqServeSub*)po.box;
        if (!ss) { quic_client_stream_reset(st, kStreamInternalError); break; }
        if (ss->catalog_json) serve_catalog_on_stream(ss, st);
        else                  serve_group_open_on_stream(ss, st);
        break;
    }
    default:
        /* kOpenSetup（uni，不走这条）、kOpenAnnounceReply / kOpenTrackInfo
         * （当前设计里对端开的请求流本来就可写，回复直接在解析里写，不排
         * 队列）。真走到这里说明队列错位了，reset 掉比静默挂住好。 */
        LOG_WARN("[moq-lite-cli] unexpected stream-open intent %d", (int)po.kind);
        quic_client_stream_reset(st, kStreamInternalError);
        break;
    }
}

static void on_session_stream_data(quic_client_t *c, quic_stream_t *st,
                                   const uint8_t *data, size_t len, void *user) {
    (void)c;
    MoqSession *s = (MoqSession*)user;
    if (!s || s->dead) return;

    MoqStream *r = stream_find(s, st);
    if (!r) {
        /* 对端开的流：第一次见到就建记录，具体属别在解析 STREAM_TYPE 时定。 */
        int uni = quic_client_stream_is_uni(st);
        r = stream_add(s, st, uni ? kStreamGroup : kStreamAnnounce, 0);
        if (!r) return;
    }
    if (r->done) return;

    if (data && len) {
        if (r->buf.len + len > MOQ_MAX_RECV_BUF) {
            LOG_WARN("[moq-lite-cli] recv buffer over max message size — resetting "
                     "the stream (not the session)");
            quic_client_stream_reset(st, kStreamInternalError);
            r->done = 1;
            moq_buf_clear(&r->buf);
            return;
        }
        if (moq_buf_append(&r->buf, data, len) < 0) {
            LOG_ERROR("[moq-lite-cli] OOM buffering stream data");
            return;
        }
    }
    stream_parse(r);

    /* 收尾即回收 —— 组流数量与帧数同量级，不回收就是无界增长。
     * 放在**解析之后**：解析过程中 r 还在用（r->buf 是解析的工作区）。 */
    if (r->done) stream_remove(s, r);
}

static void on_session_close(quic_client_t *c, int err, void *user) {
    (void)c;
    MoqSession *s = (MoqSession*)user;
    if (!s) return;
    s->dead = 1;
    for (size_t i = 0; i < s->bc_cnt; i++)
        if (s->bcs[i]) s->bcs[i]->dead = 1;
    if (s->cb.on_close) s->cb.on_close(s->slot, err, moq_lite_strerror(err),
                                       s->cb.user);
}

static void on_session_stream_reset(quic_client_t *c, quic_stream_t *st,
                                    uint64_t error_code, void *user) {
    (void)c;
    MoqSession *s = (MoqSession*)user;
    if (!s || s->dead) return;

    /* 服务端拒绝订阅就是 reset 那条 subscribe 流（-06 没有拒绝消息）。这时
     * **不能默默不管**：订阅永远不会开始，而调用方还在等 on_subscribe。
     * 用 on_drop 报出去 —— 语义最接近（这条订阅没能拿到任何组）。
     * ⚠️ 判据是「这条流属于哪个订阅」，不是「sub->started」：TRACK 流被
     * reset（服务端没 catalog / 没这条轨）同样意味着这个订阅完了。 */
    for (size_t i = 0; i < g_ctx.sub_cnt; i++) {
        MoqSub *sub = g_ctx.subs[i];
        if (!sub || !sub->used || sub->sess != s || sub->dead) continue;
        int mine = (st && (sub->sub_stream == st || sub->track_stream == st));
        if (!mine) continue;
        LOG_WARN("[moq-lite-cli] sub %u: peer reset stream (code=0x%llx) — "
                 "subscription rejected", sub->sub_id,
                 (unsigned long long)error_code);
        sub->dead = 1;
        if (sub->cb.on_drop)
            sub->cb.on_drop(sub->slot, 0, 0, error_code, sub->cb.user);
        return;
    }
}

/* ============================================================
 * 公开 API
 * ============================================================ */

moq_lite_session_t moq_lite_session_new(uv_loop_t *loop) {
    /* 循环是**调用方给的**：这条 loop 上还有他的定时器 / 信号 / 别的连接，
     * 本库既不该替他挑一条，也不该替他跑。 */
    if (!loop) { err_set(MOQ_LITE_ERR_ARGS, "loop is null"); return 0; }

    MoqSession *s = (MoqSession*)calloc(1, sizeof(*s));
    if (!s) { err_set(MOQ_LITE_ERR_NOMEM, "oom"); return 0; }
    s->loop = loop;

    pthread_mutex_lock(&g_ctx_mutex);
    size_t slot = alloc_slot((void***)&g_ctx.sessions, &g_ctx.session_cnt,
                             &g_ctx.session_cap, s, sizeof(MoqSession*));
    pthread_mutex_unlock(&g_ctx_mutex);
    if (slot == (size_t)-1) { free(s); err_set(MOQ_LITE_ERR_NOMEM, "oom"); return 0; }

    s->slot = (uint32_t)slot;
    s->used = 1;
    return (moq_lite_session_t)slot;
}

int moq_lite_session_set_alpn(const char *const *protos, int count) {
    pthread_mutex_lock(&g_ctx_mutex);
    if (g_ctx.alpn) {
        for (int i = 0; i < g_ctx.alpn_count; i++) free(g_ctx.alpn[i]);
        free(g_ctx.alpn);
        g_ctx.alpn = NULL;
        g_ctx.alpn_count = 0;
    }
    if (protos && count > 0) {
        char **copy = (char**)calloc((size_t)count, sizeof(char*));
        int n = 0;
        if (copy) {
            for (int i = 0; i < count; i++) {
                if (!protos[i] || !protos[i][0]) continue;
                copy[n] = strdup(protos[i]);
                if (!copy[n]) break;
                n++;
            }
            if (n > 0) { g_ctx.alpn = copy; g_ctx.alpn_count = n; }
            else free(copy);
        }
    }
    pthread_mutex_unlock(&g_ctx_mutex);
    return MOQ_LITE_OK;
}

int moq_lite_session_connect(moq_lite_session_t h, const char *url,
                             const moq_lite_session_cb_t *cb) {
    MoqSession *s = session_get(h);
    if (!s) return err_set(MOQ_LITE_ERR_ARGS, "invalid session handle");
    if (s->connecting || s->connected)
        return err_set(MOQ_LITE_ERR_STATE, "session already connecting/connected");

    char *host = NULL, *path = NULL;
    int port = 0;
    int rc = parse_url(url, &host, &port, &path);
    if (rc != MOQ_LITE_OK) return rc;

    quic_client_callbacks_t ccb;
    memset(&ccb, 0, sizeof(ccb));
    ccb.on_connect         = on_session_connected;
    ccb.on_stream_open     = on_session_bidi_open;
    ccb.on_stream_open_uni = on_session_uni_open;
    ccb.on_stream_data     = on_session_stream_data;
    ccb.on_stream_reset    = on_session_stream_reset;
    ccb.on_close           = on_session_close;
    ccb.user_data          = s;

    quic_client_t *cli = quic_client_new(s->loop, &ccb);
    if (!cli) {
        free(host); free(path);
        return err_set(MOQ_LITE_ERR_NOMEM, "cannot create quic client");
    }

    /* ALPN：默认只 offer MOQ_LITE_VERSION。**刻意不 offer h3** —— 本库不会
     * WebTransport，offer 了却不会说那个协议，症状是「连上了但没下文」。 */
    pthread_mutex_lock(&g_ctx_mutex);
    char **alpn = g_ctx.alpn;
    int alpn_count = g_ctx.alpn_count;
    pthread_mutex_unlock(&g_ctx_mutex);
    if (alpn && alpn_count > 0) {
        quic_client_set_alpn(cli, (const char *const *)alpn, alpn_count);
    } else {
        const char *one[1] = { MOQ_LITE_VERSION };
        quic_client_set_alpn(cli, one, 1);
    }

    s->cli = cli;
    s->host = host;
    s->port = port;
    s->path = path;
    if (cb) s->cb = *cb;
    s->connecting = 1;

    LOG_INFO("[moq-lite-cli] connecting to %s:%d path=\"%s\" alpn=%s",
             host, port, path[0] ? path : "(default)",
             (alpn && alpn_count) ? alpn[0] : MOQ_LITE_VERSION);

    quic_client_connect(cli, host, port);
    return MOQ_LITE_OK;
}

const char *moq_lite_session_alpn(moq_lite_session_t h) {
    MoqSession *s = session_get(h);
    if (!s || !s->cli) return "";
    return QuicConnectionGetAlpn(quic_client_conn(s->cli));
}

int moq_lite_session_announce(moq_lite_session_t h, const char *prefix) {
    (void)prefix;   /* 只发空前缀，见下 */
    MoqSession *s = session_get(h);
    if (!s) return err_set(MOQ_LITE_ERR_ARGS, "invalid session handle");
    if (!s->connected) return err_set(MOQ_LITE_ERR_STATE, "session not connected");
    if (s->announce_stream || s->announce_sent)
        return err_set(MOQ_LITE_ERR_STATE, "announce already in flight");

    /* ⚠️ 这里**恒发空前缀**，不转发调用方给的 prefix。
     * 理由：广播路由已经由 SETUP 的 Path 参数决定（服务端按它路由），而
     * ANNOUNCE_REQUEST 的 prefix 是**相对那个 scope 的绝对路径**。发空 = 「这
     * 个 scope 下的全部」，是最常见的用法；发别的值在对不上时**静默失败**
     * （对端回 active=0 的空集合、不报错），所以不做这个容易用错的旋钮。
     * 需要别的行为时再把它接出来。 */
    pending_push(kOpenAnnounce, NULL);
    quic_client_open_stream(s->cli);
    LOG_INFO("[moq-lite-cli] announce requested (prefix=\"\" = everything under "
             "the routed path)");
    return MOQ_LITE_OK;
}

void moq_lite_session_close(moq_lite_session_t h) {
    MoqSession *s = session_get(h);
    if (!s) return;
    s->closing = 1;
    s->dead = 1;
    for (size_t i = 0; i < s->bc_cnt; i++)
        if (s->bcs[i]) s->bcs[i]->dead = 1;
    if (s->cli) quic_client_close(s->cli);
}

moq_lite_sub_t moq_lite_subscribe(moq_lite_broadcast_t bh, const char *track,
                                  const moq_lite_sub_cb_t *cb) {
    MoqBc *bc = bc_get(bh);
    if (!bc || bc->dead || !bc->sess || bc->sess->dead)
        return (moq_lite_sub_t)err_set(MOQ_LITE_ERR_ARGS,
                                       "invalid or expired broadcast handle");
    if (!track || !track[0])
        return (moq_lite_sub_t)err_set(MOQ_LITE_ERR_ARGS, "empty track name");

    MoqSub *sub = (MoqSub*)calloc(1, sizeof(*sub));
    if (!sub) return (moq_lite_sub_t)err_set(MOQ_LITE_ERR_NOMEM, "oom");
    sub->used   = 1;
    sub->sess   = bc->sess;
    sub->bc     = bc;
    sub->track  = strdup(track);
    sub->phase  = kPhaseTrackReq;
    if (cb) sub->cb = *cb;
    if (!sub->track) {
        free(sub);
        return (moq_lite_sub_t)err_set(MOQ_LITE_ERR_NOMEM, "oom");
    }

    pthread_mutex_lock(&g_ctx_mutex);
    size_t slot = alloc_slot((void***)&g_ctx.subs, &g_ctx.sub_cnt, &g_ctx.sub_cap,
                             sub, sizeof(MoqSub*));
    pthread_mutex_unlock(&g_ctx_mutex);
    if (slot == (size_t)-1) {
        free(sub->track); free(sub);
        return (moq_lite_sub_t)err_set(MOQ_LITE_ERR_NOMEM, "oom");
    }
    sub->slot   = (uint32_t)slot;
    sub->sub_id = slot;   /* 会话内唯一，且 slot 不复用 —— 不会撞号 */

    LOG_INFO("[moq-lite-cli] subscribe: broadcast=\"%s\" track=\"%s\" sub_id=%u",
             bc->path ? bc->path : "", track, sub->sub_id);

    /* 先开 Track 流（bidi 0x6），拿到 TRACK_INFO 再开 Subscribe 流。
     * 漏掉这一步的话对端一个 SUBSCRIBE 都不会收到（见文件头）。
     * sub_open_track 自己会 push 意图，不必在这里再来一次。 */
    sub_open_track(sub);
    return (moq_lite_sub_t)slot;
}

void moq_lite_unsubscribe(moq_lite_sub_t h) {
    MoqSub *sub = sub_get(h);
    if (!sub || sub->dead) return;
    sub->dead = 1;
    /* 用 reset 而不是 close(FIN) 表达「我不订了」：语义明确，而且避开
     * 「零长 FIN 被丢」那个坑（见 quic-zero-length-fin-dropped）。 */
    if (sub->sub_stream)
        quic_client_stream_reset(sub->sub_stream, kStreamCancelled);
    if (sub->track_stream)
        quic_client_stream_reset(sub->track_stream, kStreamCancelled);
}

/* ============================================================
 * 发布（推流）—— 公开 API
 * ============================================================ */

int moq_lite_session_set_role(moq_lite_session_t h, moq_lite_role_t role) {
    MoqSession *s = session_get(h);
    if (!s) return err_set(MOQ_LITE_ERR_ARGS, "invalid session handle");
    if (s->connecting || s->connected)
        return err_set(MOQ_LITE_ERR_STATE, "role must be set before connect");
    if (role != MOQ_LITE_ROLE_BOTH && role != MOQ_LITE_ROLE_PUBLISHER &&
        role != MOQ_LITE_ROLE_SUBSCRIBER)
        return err_set(MOQ_LITE_ERR_ARGS, "bad role");
    s->role = (int)role;
    return MOQ_LITE_OK;
}

/* 十六进制小写、无 0x、无分隔符 —— catalog 的 description 字段就这个格式
 * （⚠️ 隔壁 cmaf 容器用 base64，两者别混）。 */
static int hex_encode(MoqBuf *out, const uint8_t *p, size_t n) {
    static const char d[] = "0123456789abcdef";
    if (moq_buf_reserve(out, n * 2) < 0) return -1;
    for (size_t i = 0; i < n; i++) {
        out->buf[out->len++] = (uint8_t)d[p[i] >> 4];
        out->buf[out->len++] = (uint8_t)d[p[i] & 0x0f];
    }
    return 0;
}

/* JSON 字符串转义。轨名/codec 串都是我们自己给的字面量，但仍要转义 ——
 * 名字里出现一个引号就能把整份 catalog 弄成非法 JSON（对端会静默解不开）。 */
static int json_str(MoqBuf *out, const char *s) {
    if (moq_buf_append(out, "\"", 1) < 0) return -1;
    for (const char *p = s; p && *p; p++) {
        char esc[8];
        const char *w = esc;
        switch (*p) {
        case '"':  w = "\\\""; break;
        case '\\': w = "\\\\"; break;
        case '\n': w = "\\n";  break;
        case '\r': w = "\\r";  break;
        case '\t': w = "\\t";  break;
        default:
            if ((unsigned char)*p < 0x20) {
                snprintf(esc, sizeof(esc), "\\u%04x", (unsigned)(unsigned char)*p);
            } else {
                esc[0] = *p; esc[1] = '\0';
            }
        }
        if (moq_buf_append(out, w, strlen(w)) < 0) return -1;
    }
    return moq_buf_append(out, "\"", 1);
}

static int json_u32(MoqBuf *out, const char *key, uint32_t v) {
    char buf[64];
    int n = snprintf(buf, sizeof(buf), "\"%s\":%u", key, (unsigned)v);
    return (n > 0) ? moq_buf_append(out, buf, (size_t)n) : -1;
}

/* 重建 catalog.json。
 *
 * ⚠️ 这个形状是**对端消费它时的期望**，不是我编的（服务端 OnCatalogFrame 读的就是
 * 这些键）：顶层 video/audio 各一个对象，里面 renditions 的**键就是轨名**，
 * 每条 rendition 带 codec / description(hex 配置) / 尺寸或采样率 / container。
 * clock 是可选节（timescale 通常 1000000）。 */
static int pub_build_catalog(MoqPub *pub) {
    MoqBuf b; moq_buf_init(&b);
    int rc = moq_buf_append(&b, "{", 1);
    int first_kind = 1;

    const char *kinds[2] = { "video", "audio" };
    for (int k = 0; k < 2 && rc == 0; k++) {
        const int is_video = (k == 0);
        /* 这一节里第一条轨之前要先写 `"video":{"renditions":{`。没有轨的节
         * 整个不写 —— 空 renditions 对象会让有的实现当成「有这个轨道种类但
         * 没有内容」，不如不出现。 */
        int first_track = 1;
        for (int i = 0; i < pub->track_cnt && rc == 0; i++) {
            MoqPubTrack *t = pub->tracks[i];
            if (!t || t->is_video != is_video) continue;

            if (first_track) {
                if (rc == 0 && !first_kind) rc = moq_buf_append(&b, ",", 1);
                if (rc == 0) rc = moq_buf_append(&b, "\"", 1);
                if (rc == 0) rc = moq_buf_append(&b, kinds[k], strlen(kinds[k]));
                if (rc == 0) rc = moq_buf_append(&b, "\":{\"renditions\":{", strlen("\":{\"renditions\":{"));
                first_kind  = 0;
                first_track = 0;
            } else {
                rc = moq_buf_append(&b, ",", 1);
            }
            if (rc == 0) rc = json_str(&b, t->name);
            if (rc == 0) rc = moq_buf_append(&b, ":{", 2);
            if (rc == 0) rc = moq_buf_append(&b, "\"codec\":", 8);
            if (rc == 0) rc = json_str(&b, t->codec ? t->codec : "");
            if (rc == 0 && t->config_len > 0) {
                /* description 是**十六进制**的 avcC / ASC 原字节（小写、无 0x、
                 * 无分隔符）—— 下游靠它建解码器。 */
                rc = moq_buf_append(&b, ",\"description\":\"", 16);
                if (rc == 0) rc = hex_encode(&b, t->config, t->config_len);
                if (rc == 0) rc = moq_buf_append(&b, "\"", 1);
            }
            if (rc == 0 && is_video) {
                rc = moq_buf_append(&b, ",", 1);
                if (rc == 0) rc = json_u32(&b, "codedWidth", t->width);
                if (rc == 0) rc = moq_buf_append(&b, ",", 1);
                if (rc == 0) rc = json_u32(&b, "codedHeight", t->height);
                if (rc == 0 && t->bitrate) {
                    rc = moq_buf_append(&b, ",", 1);
                    if (rc == 0) rc = json_u32(&b, "bitrate", t->bitrate);
                }
            } else if (rc == 0) {
                rc = moq_buf_append(&b, ",", 1);
                if (rc == 0) rc = json_u32(&b, "sampleRate", t->sample_rate);
                if (rc == 0) rc = moq_buf_append(&b, ",", 1);
                if (rc == 0) rc = json_u32(&b, "numberOfChannels", t->channels);
                if (rc == 0 && t->bitrate) {
                    rc = moq_buf_append(&b, ",", 1);
                    if (rc == 0) rc = json_u32(&b, "bitrate", t->bitrate);
                }
            }
            /* 关这条 rendition 的对象。 */
            if (rc == 0) rc = moq_buf_append(&b, "}", 1);
        }
        /* 关这一节：`"video":{"renditions":{ … }}` —— 先关 renditions，
         * 再关 video 那一层。没有轨的节上面整个没写，这里也不要补。 */
        if (!first_track && rc == 0) rc = moq_buf_append(&b, "}}", 2);
    }

    /* clock：这条 broadcast 的时间轴原点。timescale 与 TRACK_INFO 报的一致
     * （见 MOQ_SERVE_TIMESCALE）。 */
    if (rc == 0) {
        char clk[128];
        int n = snprintf(clk, sizeof(clk),
                         ",\"clock\":{\"wall\":%llu,\"timescale\":%d}}",
                         (unsigned long long)0, MOQ_SERVE_TIMESCALE);
        if (n > 0) rc = moq_buf_append(&b, clk, (size_t)n);
    }

    if (rc != 0) { moq_buf_free(&b); return -1; }
    moq_buf_free(&pub->catalog);
    pub->catalog = b;   /* 转移所有权 */
    return 0;
}

/* ⚠️ 失败一律返回 **0**，不是错误码 —— 返回值是**句柄**，把 -3 强转成
 * uint32_t 会变成一个巨大的非零值，调用方的 `if (!h)` 判不出来（实测踩过：
 * catalog 构建失败被当成「发布成功」，然后在订阅应答里静默什么都不发）。
 * 错误原因放在线程局部的 moq_lite_strerror 里。 */
moq_lite_broadcast_t moq_lite_publish(moq_lite_session_t h, const char *path) {
    MoqSession *s = session_get(h);
    if (!s) { err_set(MOQ_LITE_ERR_ARGS, "bad session"); return 0; }
    if (!s->connected) { err_set(MOQ_LITE_ERR_STATE, "not connected"); return 0; }
    if (!path || !path[0]) { err_set(MOQ_LITE_ERR_ARGS, "empty path"); return 0; }

    MoqPub *pub = (MoqPub*)calloc(1, sizeof(*pub));
    if (!pub) { err_set(MOQ_LITE_ERR_NOMEM, "oom"); return 0; }
    pub->path = strdup(path);
    pub->sess = s;
    pub->used = 1;
    moq_buf_init(&pub->catalog);
    if (!pub->path) {
        free(pub);
        err_set(MOQ_LITE_ERR_NOMEM, "oom");
        return 0;
    }
    /* 句柄要在**进程级**表里登记（moq_lite_broadcast_t 是进程级的），同时把
     * 指针记进会话的列表供遍历（应答 announce / 查 catalog 都要扫它）。
     * ⚠️ 两处都要 —— 只登记一处的话，句柄能建出来但 pub_get 查不到
     * （返回的 slot 是另一张表的下标），症状是 publish 成功、publish_track
     * 报「invalid handle」。 */
    size_t slot = alloc_slot((void***)&g_ctx.pubs, &g_ctx.pub_cnt, &g_ctx.pub_cap,
                             pub, sizeof(MoqPub*));
    if (slot == (size_t)-1 ||
        alloc_slot((void***)&s->pubs, &s->pub_cnt, &s->pub_cap, pub,
                   sizeof(MoqPub*)) == (size_t)-1) {
        free(pub->path); free(pub);
        err_set(MOQ_LITE_ERR_NOMEM, "oom");
        return 0;
    }
    pub->slot = (uint32_t)slot;
    LOG_INFO("[moq-lite-cli] publish broadcast \"%s\" declared", path);
    return (moq_lite_broadcast_t)slot;
}

moq_lite_track_t moq_lite_publish_track(moq_lite_broadcast_t bh, int is_video,
                                        const moq_lite_rendition_t *r) {
    MoqPub *pub = NULL;
    /* pub 句柄是会话内的，不是全局的 —— 按 slot 反查要遍历会话表。
     * 这里用一个小的注册表把 pub 也放到全局，句柄才能跨函数稳定解引用。 */
    pub = pub_get(bh);
    if (!pub || pub->dead || !pub->sess) {
        err_set(MOQ_LITE_ERR_ARGS, "bad broadcast"); return 0;
    }
    if (!r || !r->name || !r->name[0] || !r->codec || !r->codec[0]) {
        err_set(MOQ_LITE_ERR_ARGS, "missing track name/codec"); return 0;
    }
    if (pub->track_cnt >= MOQ_MAX_PUB_TRACKS) {
        err_set(MOQ_LITE_ERR_STATE, "too many tracks"); return 0;
    }

    MoqPubTrack *t = (MoqPubTrack*)calloc(1, sizeof(*t));
    if (!t) { err_set(MOQ_LITE_ERR_NOMEM, "oom"); return 0; }
    t->used = 1;
    t->is_video = is_video ? 1 : 0;
    t->pub = pub;
    t->name = strdup(r->name);
    t->codec = strdup(r->codec);
    if (r->config_len && r->config) {
        t->config = (uint8_t*)malloc(r->config_len);
        if (t->config) { memcpy(t->config, r->config, r->config_len);
                         t->config_len = r->config_len; }
    }
    t->width = r->width; t->height = r->height;
    t->sample_rate = r->sample_rate; t->channels = r->channels;
    t->bitrate = r->bitrate;
    if (!t->name || !t->codec) {
        free(t->name); free(t->codec); free(t);
        err_set(MOQ_LITE_ERR_NOMEM, "oom");
        return 0;
    }

    size_t slot = alloc_slot((void***)&g_ctx.tracks, &g_ctx.track_cnt,
                             &g_ctx.track_cap, t, sizeof(MoqPubTrack*));
    if (slot == (size_t)-1) {
        free(t->name); free(t->codec); free(t->config); free(t);
        err_set(MOQ_LITE_ERR_NOMEM, "oom");
        return 0;
    }
    t->slot = (uint32_t)slot;
    pub->tracks[pub->track_cnt++] = t;

    /* catalog 是派生的，轨一变就重建。对端订阅 catalog.json 时读的就是它。 */
    if (pub_build_catalog(pub) < 0) {
        err_set(MOQ_LITE_ERR_NOMEM, "cannot build catalog");
        return 0;
    }
    LOG_INFO("[moq-lite-cli] publish track \"%s\" (%s) codec=%s config=%zu bytes",
             t->name, is_video ? "video" : "audio", t->codec, t->config_len);
    return (moq_lite_track_t)slot;
}

/* ── 保留缓冲：新订阅方的起步点 ─────────────────
 *
 * 上限只是内存闸：一个 GOP（10 秒 × 25 fps × 2 KB）约 0.5 MB，正常碰不到。
 * 真碰到了就整段丢掉等下一个关键帧 —— 视频不能从半路开始放。 */
#define MOQ_HOLD_MAX_BYTES (8u * 1024 * 1024)

static void hold_clear(MoqPubTrack *t) {
    for (size_t i = 0; i < t->hold_cnt; i++) free(t->hold[i].data);
    t->hold_cnt = 0;
    t->hold_bytes = 0;
}

static int hold_push(MoqPubTrack *t, uint64_t ts_us,
                     const uint8_t *payload, size_t len) {
    if (t->hold_cnt == t->hold_cap) {
        size_t want = t->hold_cap ? t->hold_cap * 2 : 64;
        ServeFrame *nb = (ServeFrame*)realloc(t->hold, want * sizeof(*nb));
        if (!nb) return -1;
        t->hold = nb;
        t->hold_cap = want;
    }
    uint8_t *copy = (uint8_t*)malloc(len ? len : 1);
    if (!copy) return -1;
    if (len) memcpy(copy, payload, len);
    t->hold[t->hold_cnt].ts_us = ts_us;
    t->hold[t->hold_cnt].data  = copy;
    t->hold[t->hold_cnt].len   = len;
    t->hold_cnt++;
    t->hold_bytes += len;
    return 0;
}

/* 音频只留最近约 1 秒（与组节奏一致）：既不空手起步，也不拖一段陈旧的。 */
static void hold_trim_audio(MoqPubTrack *t) {
    while (t->hold_cnt > 0 &&
           t->hold[0].ts_us + MOQ_SERVE_AUDIO_GROUP_US <
               t->hold[t->hold_cnt - 1].ts_us) {
        t->hold_bytes -= t->hold[0].len;
        free(t->hold[0].data);
        memmove(&t->hold[0], &t->hold[1],
                (t->hold_cnt - 1) * sizeof(ServeFrame));
        t->hold_cnt--;
    }
}

/* 把保留缓冲放给一条**新**订阅 —— 视频那边第一帧就是关键帧，所以它会开组。 */
static void serve_replay_hold(MoqServeSub *ss, MoqPubTrack *t) {
    if (t->hold_cnt == 0) return;
    LOG_INFO("[moq-lite-cli] sub %llu: starting from the held buffer "
             "(%zu frames, %llu bytes)",
             (unsigned long long)ss->sid, t->hold_cnt,
             (unsigned long long)t->hold_bytes);
    for (size_t i = 0; i < t->hold_cnt; i++)
        serve_write_frame(ss, t->hold[i].ts_us, t->hold[i].data, t->hold[i].len);
}

/* 把一帧分发给**所有**订阅了这条轨的订阅方。
 * 组边界：视频只在关键帧处开新组（组内非关键帧若还没开组就丢）；
 * 音频按 1 秒切一组。 */
int moq_lite_track_frame(moq_lite_track_t th, uint64_t ts_us,
                         const uint8_t *payload, size_t len, int key) {
    MoqPubTrack *t = track_get(th);
    if (!t || t->dead) return err_set(MOQ_LITE_ERR_ARGS, "bad track");
    if (!payload || len == 0) return err_set(MOQ_LITE_ERR_ARGS, "empty frame");
    MoqPub *pub = t->pub;
    MoqSession *s = pub->sess;

    for (size_t i = 0; i < s->serve_cnt; i++) {
        MoqServeSub *ss = s->serve_subs[i];
        if (!ss || !ss->used || ss->dead || ss->track != t) continue;

        if (t->is_video) {
            if (key) {
                /* 关键帧 = 组边界（硬要求）：关掉当前组，这一帧开新组。
                 * 组里没有关键帧就解不出来，所以组必须跟着关键帧走 —— 不轮转
                 * 的话整条轨就一个大组，后加入的订阅方永远等不到可解码的起点。 */
                if (ss->uni || ss->group_opening) serve_group_close(ss);
            } else if (!ss->uni && !ss->group_opening) {
                continue;   /* 还没开组又非关键帧：丢（组里必须有关键帧打头） */
            }
        } else {
            /* 音频：跨过一个秒界就关旧组（FIN），下一帧开新组。 */
            if (ss->uni && (int64_t)ts_us >= ss->next_audio_us)
                serve_group_close(ss);
            if (!ss->uni && !ss->group_opening)
                ss->next_audio_us = (int64_t)ts_us + MOQ_SERVE_AUDIO_GROUP_US;
        }
        serve_write_frame(ss, ts_us, payload, len);
    }

    /* 维护保留缓冲，给**将来**的订阅方用（见 MoqPubTrack::hold 的说明）。 */
    if (t->is_video) {
        if (key) hold_clear(t);        /* 关键帧 = 新的起步点 */
        if (hold_push(t, ts_us, payload, len) < 0) hold_clear(t);
    } else {
        if (hold_push(t, ts_us, payload, len) == 0) hold_trim_audio(t);
    }
    if (t->hold_bytes > MOQ_HOLD_MAX_BYTES) {
        LOG_WARN("[moq-lite-cli] track \"%s\": hold buffer over %u MB — "
                 "dropping it, next subscriber waits for a keyframe",
                 t->name, MOQ_HOLD_MAX_BYTES / (1024 * 1024));
        hold_clear(t);
    }
    return MOQ_LITE_OK;
}

void moq_lite_track_end(moq_lite_track_t th) {
    MoqPubTrack *t = track_get(th);
    if (!t || t->dead) return;
    MoqPub *pub = t->pub;
    for (size_t i = 0; i < pub->sess->serve_cnt; i++) {
        MoqServeSub *ss = pub->sess->serve_subs[i];
        if (ss && ss->used && !ss->dead && ss->track == t)
            serve_group_close(ss);
    }
    hold_clear(t);
    t->dead = 1;
}

/* ============================================================
 * 事件循环
 *
 * 循环**完全归调用方**：loop 由 moq_lite_session_new 传入，本库既不挑一条、
 * 也不提供驱动接口。理由很实在 —— 那条 loop 上通常还有调用方自己的定时器、
 * 信号处理、别的连接，本库无从知道「跑到什么时候算完」。所以收尾就是调用方
 * 自己 `uv_run(loop, UV_RUN_DEFAULT)`（或 UV_RUN_ONCE / NOWAIT），
 * 在回调里想退出就 `uv_stop(loop)`。
 * ============================================================ */
