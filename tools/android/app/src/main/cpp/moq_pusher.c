#include "moq_pusher.h"
#include "moq_lite_client.h"
#include "moq_log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uv.h>


/* 队列上限（条）。MediaCodec 正常 25~60 帧/秒，这个深度够吸收几秒抖动；
 * 真撞上了说明 loop 线程卡住了 —— 丢帧并计数，别让内存无界涨。 */
#define PQ_MAX_ITEMS 256

/* 编解码参数暂存上限。avcC 几十字节、ASC 通常 2 字节，256 足够。 */
#define PQ_CFG_MAX 256

typedef enum {
    PQ_VIDEO_CFG = 1,
    PQ_AUDIO_CFG,
    PQ_VIDEO_FRAME,
    PQ_AUDIO_FRAME,
    PQ_FINISH,          /* 收尾：关组、补 FIN，但不断连接 */
    PQ_QUIT,            /* 收尾并停 loop */
} pq_kind_t;

typedef struct pq_item {
    struct pq_item *next;
    pq_kind_t       kind;
    uint64_t        ts_us;
    int             key;
    uint8_t        *data;      /* 帧载荷 / 编解码参数（都是拷贝，见下） */
    size_t          len;
    uint32_t        a, b;      /* video: w,h   audio: rate,ch */
    char            track[64];
} pq_item_t;

/* 一条轨的编解码参数（loop 线程私有，由 PQ_*_CFG 攒出来） */
typedef struct {
    int      have;
    int      published;
    uint8_t  cfg[PQ_CFG_MAX];
    size_t   cfg_len;
    uint32_t a, b;
    char     track[64];
} track_cfg_t;

typedef struct {
    /* 线程与 loop */
    uv_thread_t thread;
    uv_loop_t   loop;
    uv_async_t  async;
    uv_sem_t    init_sem;       /* start() 等 loop 线程把 async 建好 */
    int         thread_started;
    int         loop_alive;     /* 受 lock 保护：async 句柄此刻是否还可 send */

    /* 跨线程状态（全部受 lock 保护） */
    uv_mutex_t  lock;
    pq_item_t  *head, *tail;
    int         qlen;
    int         stop_requested;
    uint64_t    dropped;        /* 队列满丢弃 + 轨未就绪丢弃，都记这里 */
    uint64_t    v_frames, a_frames;
    int         stats_connected;
    int         stats_published;

    /* 只写一次、之后只读，不必加锁 */
    char        url[256];
    char        broadcast[128];

    /* ── 以下仅 loop 线程访问 ── */
    moq_lite_session_t   sess;
    moq_lite_broadcast_t bc;
    moq_lite_track_t     vt, at;
    int                  connected;
    track_cfg_t          vcfg, acfg;
} moq_pusher_t;

static moq_pusher_t g;

/* 互斥量/信号量只初始化**一次**（JNI_OnLoad），之后整个进程生命周期都不销毁。
 * 不销毁是因为 UI 线程一直在轮询 get_stats —— stop() 之后它还会继续调，
 * 那时去 lock 一个已 destroy 的 mutex 就是 UB。这个对象只有一个，留着不占什么。 */
static volatile int g_sync_ready = 0;

int moq_pusher_global_init(void) {
    if (g_sync_ready) return 0;

    /* ── 关掉 libuv 的 io_uring（Android 上不关就崩）──────────────
     *
     * ⚠️ 这是 Android 上**每次 uv_loop_init 都会踩**的坑，不是可选项。
     *
     * libuv 在 Linux 上建 loop 时会**无条件**试两条 io_uring 环
     * （src/unix/linux.c:547-548 的 uv__iou_init）：
     *     uv_loop_init → uv__platform_loop_init → uv__iou_init
     *                 → uv__io_uring_setup → syscall(__NR_io_uring_setup, …)
     * 而 **Android 的 seccomp 策略不允许 app 用 io_uring** —— 那个 syscall
     * 不是返回错误，而是直接把进程打死（收到信号），栈回溯就停在
     * `syscall(__NR_io_uring_setup, …)` 那一行，看着像「uv_loop_init 里崩了」。
     *
     * libuv 自己留了开关：`uv__use_io_uring()` 读环境变量 UV_USE_IO_URING，
     * 值为 "0" 就整个跳过（它内部把结果缓存在一个 static 里，所以
     * **必须在这条进程第一次 uv_loop_init 之前设**）。
     *
     * 为什么用环境变量而不是改 3rdparty/libuv：那是 vendored 的上游代码，
     * 打补丁会在下次同步上游时丢掉，而这里本来就是 libuv 官方给的出口。
     * 代价只有一点：setenv 是进程级的，会一并影响同进程里别的 libuv 使用者 ——
     * 在 Android 上反正谁都用不了 io_uring，没损失。 */
    setenv("UV_USE_IO_URING", "0", 1);

    if (uv_mutex_init(&g.lock) != 0) return -1;
    if (uv_sem_init(&g.init_sem, 0) != 0) { uv_mutex_destroy(&g.lock); return -1; }
    g_sync_ready = 1;
    return 0;
}

/* 计数器的自增一律在锁内 —— get_stats 会从 UI 线程读，裸 uint64_t 的并发读写
 * 是实打实的数据竞争。每次一条锁的代价远小于拷一帧的内存带宽。 */
static void bump(uint64_t *ctr, uint64_t by) {
    uv_mutex_lock(&g.lock);
    *ctr += by;
    uv_mutex_unlock(&g.lock);
}

/* ── 队列 ─────────────────────────────────────── */

/* 入队。调用方已经持有 lock 时用 _locked 版本。 */
static void pq_push_locked(pq_item_t *it) {
    if (g.qlen >= PQ_MAX_ITEMS) {
        /* 满了丢**最老的**：直播里迟到的一帧本来就该丢，新帧更有用。 */
        pq_item_t *old = g.head;
        if (old) {
            g.head = old->next;
            if (!g.head) g.tail = NULL;
            g.qlen--;
            free(old->data);
            free(old);
        }
        g.dropped++;
    }
    it->next = NULL;
    if (!g.tail) g.head = g.tail = it;
    else { g.tail->next = it; g.tail = it; }
    g.qlen++;
}

static void pq_push(pq_item_t *it) {
    if (!it) return;
    /* 单点守卫：库加载了但还没 start 时，任何入队都直接放掉 —— 免得去 lock
     * 一个还没建的互斥量。 */
    if (!g_sync_ready) { free(it->data); free(it); return; }

    uv_mutex_lock(&g.lock);
    int alive = g.loop_alive;
    if (alive) {
        pq_push_locked(it);
        /* ⚠️ send 必须在**锁内**：loop 线程的收尾是在同一把锁里把 loop_alive
         * 置 0、随后 uv_close(async) 的。锁外 send 就可能落在 close 之后 ——
         * 对已关闭的句柄 uv_async_send 是 use-after-free。
         * uv_async_send 本身非阻塞、异步信号安全，持锁调用没有代价。 */
        uv_async_send(&g.async);
    }
    uv_mutex_unlock(&g.lock);

    /* loop 已经退出（会话死了 / 已 stop）→ 没人在取队了，这一项直接放掉。
     * ⚠️ 不能塞进队列：那样只会被「队列满丢最旧」反复腾挪，白白烧 CPU，
     * 而且记进 dropped 会让 UI 上那个 dropped 一路涨（看起来像还在推、
     * 只是丢包，其实根本没在推）。 */
    if (!alive) { free(it->data); free(it); }
}

static pq_item_t *pq_pop_all(void) {
    uv_mutex_lock(&g.lock);
    pq_item_t *h = g.head;
    g.head = g.tail = NULL;
    g.qlen = 0;
    uv_mutex_unlock(&g.lock);
    return h;
}

static void pq_item_free(pq_item_t *it) {
    if (!it) return;
    free(it->data);
    free(it);
}

/* 造一条带拷贝的队列项。p 为 NULL 时只造空壳（QUIT/FINISH 用）。 */
static pq_item_t *pq_item_new(pq_kind_t kind, const uint8_t *p, size_t len) {
    pq_item_t *it = (pq_item_t *)calloc(1, sizeof(*it));
    if (!it) return NULL;
    if (p && len) {
        it->data = (uint8_t *)malloc(len);
        if (!it->data) { free(it); return NULL; }
        memcpy(it->data, p, len);
    }
    it->kind = kind;
    it->len = len;
    return it;
}

/* ── 发布 ─────────────────────────────────────── */

/* 从 avcC 里取 profile/compat/level 拼 codec 串（avcC[1..3]）。
 * 拿不到就退回一个 baseline 的常见值 —— codec 串只是给下游挑解码器用的。 */
static void video_codec_str(const uint8_t *avcc, size_t n, char *out, size_t cap) {
    if (avcc && n >= 4)
        snprintf(out, cap, "avc1.%02x%02x%02x", avcc[1], avcc[2], avcc[3]);
    else
        snprintf(out, cap, "avc1.42c01e");
}

/* connect 成功、或某条轨的参数刚到 —— 任一个就绪就试一次。只在 loop 线程调。 */
static void try_publish(void) {
    if (!g.sess || !g.connected) return;

    if (!g.bc) {
        g.bc = moq_lite_publish(g.sess, g.broadcast);
        if (!g.bc) {
            /* 返回值是**句柄**，失败时是 0（不是负错误码）—— 见头文件说明。 */
            LOGE("publish(\"%s\") failed", g.broadcast);
            return;
        }
        LOGI("broadcast \"%s\" declared", g.broadcast);
    }

    if (g.vcfg.have && !g.vcfg.published) {
        char codec[32];
        video_codec_str(g.vcfg.cfg, g.vcfg.cfg_len, codec, sizeof(codec));
        moq_lite_rendition_t r; memset(&r, 0, sizeof(r));
        r.name = g.vcfg.track[0] ? g.vcfg.track : "0.h264";
        r.codec = codec;
        r.config = g.vcfg.cfg;
        r.config_len = g.vcfg.cfg_len;
        r.width = g.vcfg.a;
        r.height = g.vcfg.b;
        g.vt = moq_lite_publish_track(g.bc, 1, &r);
        if (g.vt) {
            g.vcfg.published = 1;
            LOGI("video track \"%s\" (%s %ux%u, avcC %zu B)",
                 r.name, codec, r.width, r.height, r.config_len);
        } else {
            LOGE("publish_track(video) failed");
        }
    }

    if (g.acfg.have && !g.acfg.published) {
        moq_lite_rendition_t r; memset(&r, 0, sizeof(r));
        r.name = g.acfg.track[0] ? g.acfg.track : "1.aac";
        r.codec = "mp4a.40.2";
        r.config = g.acfg.cfg;
        r.config_len = g.acfg.cfg_len;
        r.sample_rate = g.acfg.a;
        r.channels = g.acfg.b;
        g.at = moq_lite_publish_track(g.bc, 0, &r);
        if (g.at) {
            g.acfg.published = 1;
            LOGI("audio track \"%s\" (%u Hz %uch, ASC %zu B)",
                 r.name, r.sample_rate, r.channels, r.config_len);
        } else {
            LOGE("publish_track(audio) failed");
        }
    }

    if (g.vt || g.at) {
        uv_mutex_lock(&g.lock);
        g.stats_published = 1;
        uv_mutex_unlock(&g.lock);
    }
}

/* ── 会话回调（都在 loop 线程）────────────────── */

static void on_connected(moq_lite_session_t s, int status, void *user) {
    (void)user;
    if (status != 0) {
        LOGE("connect failed: %d (%s)", status, moq_lite_strerror(status));
        uv_stop(&g.loop);
        return;
    }
    g.connected = 1;
    uv_mutex_lock(&g.lock);
    g.stats_connected = 1;
    uv_mutex_unlock(&g.lock);

    /* ⚠️ alpn 一定要看一眼：不对（比如协商到 h3）说明对端把我们当成
     * WebTransport，症状是「连上了但没有任何下文」，非常难查。 */
    LOGI("connected, alpn=\"%s\"", moq_lite_session_alpn(s));
    try_publish();
}

static void on_close(moq_lite_session_t s, int err, const char *reason, void *user) {
    (void)s; (void)user;
    LOGI("session closed: err=%d (%s)", err, reason ? reason : "");
    g.connected = 0;
    uv_mutex_lock(&g.lock);
    g.stats_connected = 0;
    /* ⚠️ published 也要清。它是「广播已公告」的意思，会话都没了就该回到 0 ——
     * 不清的话 UI 会一直显示 connected=false / published=true 这种自相矛盾的
     * 状态，让人以为还在推。 */
    g.stats_published = 0;
    uv_mutex_unlock(&g.lock);
    /* 会话没了就没得推了 —— 停 loop，交给 stop() 收尾。 */
    uv_stop(&g.loop);
}

/* ── loop 线程 ────────────────────────────────── */

static void drain(void) {
    for (pq_item_t *it = pq_pop_all(); it; ) {
        pq_item_t *next = it->next;

        switch (it->kind) {
        case PQ_VIDEO_CFG:
            if (it->len <= PQ_CFG_MAX) {
                memcpy(g.vcfg.cfg, it->data, it->len);
                g.vcfg.cfg_len = it->len;
                g.vcfg.have    = 1;
                g.vcfg.a = it->a; g.vcfg.b = it->b;
                snprintf(g.vcfg.track, sizeof(g.vcfg.track), "%s", it->track);
                try_publish();
            }
            break;

        case PQ_AUDIO_CFG:
            if (it->len <= PQ_CFG_MAX) {
                memcpy(g.acfg.cfg, it->data, it->len);
                g.acfg.cfg_len = it->len;
                g.acfg.have    = 1;
                g.acfg.a = it->a; g.acfg.b = it->b;
                snprintf(g.acfg.track, sizeof(g.acfg.track), "%s", it->track);
                try_publish();
            }
            break;

        case PQ_VIDEO_FRAME:
            /* 轨还没声明（参数没到 / 还没连上）→ 丢。视频没有 avcC 本来就
             * 没意义，而且 API 要求 track 句柄有效。 */
            if (g.vt && moq_lite_track_frame(g.vt, it->ts_us, it->data, it->len,
                                             it->key) == MOQ_LITE_OK)
                bump(&g.v_frames, 1);
            else
                bump(&g.dropped, 1);
            break;

        case PQ_AUDIO_FRAME:
            if (g.at && moq_lite_track_frame(g.at, it->ts_us, it->data, it->len,
                                             0) == MOQ_LITE_OK)
                bump(&g.a_frames, 1);
            else
                bump(&g.dropped, 1);
            break;

        case PQ_FINISH:
            if (g.vt) moq_lite_track_end(g.vt);
            if (g.at) moq_lite_track_end(g.at);
            break;

        case PQ_QUIT:
            if (g.vt) moq_lite_track_end(g.vt);
            if (g.at) moq_lite_track_end(g.at);
            if (g.sess) moq_lite_session_close(g.sess);
            uv_stop(&g.loop);
            break;
        }

        pq_item_free(it);
        it = next;
    }
}

static void on_async(uv_async_t *h) {
    (void)h;
    drain();
}

static void pusher_thread(void *arg) {
    (void)arg;

    /* 排障日志，保留：确认 loop 真的建起来了（上一版就是在这一步收信号的）。
     * `uv_loop_size()` 是**库里**认为的 uv_loop_t 大小，`sizeof(uv_loop_t)` 是
     * **本 TU 编出来的**大小 —— 两者不等才说明头文件与 libuv 编译配置对不上。
     * （这条已经量过：带不带 `_FILE_OFFSET_BITS=64` 都是 loop 848 / async 128，
     * 所以它不是当初那个崩溃的原因；真正的元凶是 io_uring，见 global_init。） */
    LOGI("thread entry: &g.loop=%p uv_loop_size=%zu sizeof(uv_loop_t)=%zu "
         "sizeof(uv_async_t)=%zu sizeof(g)=%zu",
         (void*)&g.loop, uv_loop_size(), sizeof(uv_loop_t),
         sizeof(uv_async_t), sizeof(g));

    int lrc = uv_loop_init(&g.loop);
    LOGI("uv_loop_init -> %d (%s)", lrc, lrc == 0 ? "ok" : uv_strerror(lrc));
    if (lrc != 0) {
        uv_sem_post(&g.init_sem);   /* 让 start() 别卡死 */
        return;
    }
    uv_async_init(&g.loop, &g.async, on_async);

    /* 会话必须在 loop 线程上建 —— loop 归调用方，回调也在这条线程。 */
    g.sess = moq_lite_session_new(&g.loop);
    if (g.sess) {
        /* ⚠️ 推流方向必须设，且必须在 connect 之前：这个参数说的是**对端是谁**，
         * 发 1 对端才把自己当订阅方来订阅我们。不设的话服务端只当我们是订阅方，
         * 我们推的东西没人订阅，而且它一声不响。 */
        moq_lite_session_set_role(g.sess, MOQ_LITE_ROLE_PUBLISHER);

        moq_lite_session_cb_t cb; memset(&cb, 0, sizeof(cb));
        cb.on_connected = on_connected;
        cb.on_close     = on_close;
        int rc = moq_lite_session_connect(g.sess, g.url, &cb);
        if (rc != MOQ_LITE_OK)
            LOGE("connect(%s) failed: %s", g.url, moq_lite_strerror(rc));
    } else {
        LOGE("session_new failed");
    }

    uv_mutex_lock(&g.lock);
    g.loop_alive = 1;
    uv_mutex_unlock(&g.lock);
    uv_sem_post(&g.init_sem);       /* 生产者现在可以安全 uv_async_send 了 */

    uv_run(&g.loop, UV_RUN_DEFAULT);

    /* ── 收尾 ──
     * ⚠️ 先把 loop_alive 置 0（同锁），再 close async —— 反过来的话，stop() 可能
     * 在 close 之后才 send，那是 use-after-free。 */
    uv_mutex_lock(&g.lock);
    int was_stop = g.stop_requested;
    g.loop_alive = 0;
    g.stats_connected = 0;
    g.stats_published = 0;
    uv_mutex_unlock(&g.lock);

    /* 队列里可能还压着没处理的帧，放掉（会话已经关了，写了也没用）。 */
    for (pq_item_t *it = pq_pop_all(); it; ) {
        pq_item_t *nx = it->next;
        pq_item_free(it);
        it = nx;
    }

    uv_close((uv_handle_t *)&g.async, NULL);
    uv_run(&g.loop, UV_RUN_NOWAIT);     /* 让 close 回调跑完 */
    uv_loop_close(&g.loop);

    LOGI("thread exit (stop_requested=%d)", was_stop);
}

/* ── 对外 ─────────────────────────────────────── */

int moq_pusher_start(const char *url, const char *broadcast) {
    if (!url || !url[0]) return -1;
    if (!g_sync_ready && moq_pusher_global_init() != 0) return -1;

    uv_mutex_lock(&g.lock);
    int already = g.thread_started;
    uv_mutex_unlock(&g.lock);
    if (already) return 0;

    /* ⚠️ **不要 memset 整个 g**：那会把已经初始化好的 mutex/sem 清掉。
     * 这里逐项重置本次 run 的状态 —— 此刻 loop 线程还没起，这些字段没人并发访问。
     * （`g.lock` / `g.init_sem` / `g.sync_ready` / `g.thread` 一律不动。） */
    g.head = g.tail = NULL;
    g.qlen = 0;
    g.stop_requested = 0;
    g.loop_alive = 0;
    g.dropped = 0;
    g.v_frames = 0;
    g.a_frames = 0;
    g.stats_connected = 0;
    g.stats_published = 0;

    /* loop 线程私有字段：上一轮可能留下非空的 bc/vt/at，不清掉的话新一轮会
     * 以为「已经 publish 过了」而跳过。线程已 join，此刻安全。 */
    g.sess = 0; g.bc = 0; g.vt = 0; g.at = 0;
    g.connected = 0;
    memset(&g.vcfg, 0, sizeof(g.vcfg));
    memset(&g.acfg, 0, sizeof(g.acfg));

    snprintf(g.url, sizeof(g.url), "%s", url);
    snprintf(g.broadcast, sizeof(g.broadcast), "%s",
             (broadcast && broadcast[0]) ? broadcast : "android.hang");

    /* 把信号量排空到 0（上一轮可能留了计数，不清的话下面的 wait 会立刻返回，
     * 而那时 loop 还没把 async 建好）。 */
    while (uv_sem_trywait(&g.init_sem) == 0) { }

    if (uv_thread_create(&g.thread, pusher_thread, NULL) != 0) {
        LOGE("uv_thread_create failed");
        return -1;
    }
    uv_mutex_lock(&g.lock);
    g.thread_started = 1;
    uv_mutex_unlock(&g.lock);

    /* 等 loop 线程把 async 建好 —— 在那之前 uv_async_send 不安全。 */
    uv_sem_wait(&g.init_sem);

    LOGI("started: url=%s broadcast=%s", g.url, g.broadcast);
    return 0;
}

void moq_pusher_stop(void) {
    if (!g_sync_ready) return;

    uv_mutex_lock(&g.lock);
    int started = g.thread_started;
    int alive   = g.loop_alive;
    g.stop_requested = 1;
    uv_mutex_unlock(&g.lock);
    if (!started) return;

    if (alive) {
        /* 入队一条 QUIT，让 loop 线程去关组、补 FIN、关会话、停 loop。 */
        pq_item_t *it = pq_item_new(PQ_QUIT, NULL, 0);
        if (it) pq_push(it);
        else     uv_stop(&g.loop);
    }

    uv_thread_join(&g.thread);

    uv_mutex_lock(&g.lock);
    g.thread_started = 0;
    uv_mutex_unlock(&g.lock);

    /* ⚠️ 不销毁 g.lock / g.init_sem —— UI 线程仍在轮询 get_stats，
     * 销掉之后它一 lock 就是 UB。整个进程只有这一对，留着。 */
    LOGI("stopped");
}

/* 收尾但不断连接：给每条订阅当前开着的组补结束哨兵 + FIN。 */
void moq_pusher_finish_tracks(void) {
    pq_item_t *it = pq_item_new(PQ_FINISH, NULL, 0);
    if (it) pq_push(it);
}

int moq_pusher_set_video_config(const uint8_t *avcc, size_t len,
                                uint32_t width, uint32_t height,
                                const char *track_name) {
    if (!avcc || len == 0 || len > PQ_CFG_MAX) return -1;
    pq_item_t *it = pq_item_new(PQ_VIDEO_CFG, avcc, len);
    if (!it) return -1;
    it->a = width;
    it->b = height;
    snprintf(it->track, sizeof(it->track), "%s",
             (track_name && track_name[0]) ? track_name : "0.h264");
    pq_push(it);
    return 0;
}

int moq_pusher_set_audio_config(const uint8_t *asc, size_t len,
                                uint32_t sample_rate, uint32_t channels,
                                const char *track_name) {
    if (!asc || len == 0 || len > PQ_CFG_MAX) return -1;
    pq_item_t *it = pq_item_new(PQ_AUDIO_CFG, asc, len);
    if (!it) return -1;
    it->a = sample_rate;
    it->b = channels;
    snprintf(it->track, sizeof(it->track), "%s",
             (track_name && track_name[0]) ? track_name : "1.aac");
    pq_push(it);
    return 0;
}

void moq_pusher_send_video(uint64_t ts_us, const uint8_t *avcc, size_t len, int key) {
    if (!avcc || len == 0) return;
    pq_item_t *it = pq_item_new(PQ_VIDEO_FRAME, avcc, len);
    if (!it) return;
    it->ts_us = ts_us;
    it->key   = key ? 1 : 0;
    pq_push(it);
}

void moq_pusher_send_audio(uint64_t ts_us, const uint8_t *aac, size_t len) {
    if (!aac || len == 0) return;
    pq_item_t *it = pq_item_new(PQ_AUDIO_FRAME, aac, len);
    if (!it) return;
    it->ts_us = ts_us;
    pq_push(it);
}

void moq_pusher_get_stats(moq_pusher_stats_t *out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    /* UI 从 Activity 一启动就轮询，那时很可能还没 start —— 互斥量虽然
     * JNI_OnLoad 时已经建好，但这条路要能扛住「库刚加载、啥都没跑」。 */
    if (!g_sync_ready) return;
    uv_mutex_lock(&g.lock);
    out->running        = g.thread_started && g.loop_alive;
    out->connected      = g.stats_connected;
    out->published      = g.stats_published;
    out->video_frames   = g.v_frames;
    out->audio_frames   = g.a_frames;
    out->dropped        = g.dropped;
    uv_mutex_unlock(&g.lock);
}
