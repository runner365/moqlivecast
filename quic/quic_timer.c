#include "quic_timer.h"
#include "logger.h"
#include <string.h>

#define QUIC_TIMER_MAX_FIRE  1024
#define QUIC_TIMER_MAX_DEFER 64

static uv_loop_t  *g_loop = NULL;
static uv_timer_t  g_uv;
static int         g_inited = 0;
static int         g_dispatching = 0;
static QuicTimer  *g_head = NULL;

static void            *g_defer_ptr[QUIC_TIMER_MAX_DEFER];
static quic_timer_free_fn g_defer_fn[QUIC_TIMER_MAX_DEFER];
static int              g_defer_n = 0;

typedef struct {
    quic_timer_cb cb;
    void         *user;
} QuicTimerFire;

static void timer_unlink(QuicTimer *t) {
    if (!t || !t->active) return;
    if (t->prev) t->prev->next = t->next;
    else         g_head = t->next;
    if (t->next) t->next->prev = t->prev;
    t->next = NULL;
    t->prev = NULL;
    t->active = 0;
}

static void timer_link_sorted(QuicTimer *t) {
    QuicTimer *cur = g_head;
    QuicTimer *prev = NULL;
    while (cur && cur->due_ms <= t->due_ms) {
        prev = cur;
        cur = cur->next;
    }
    t->prev = prev;
    t->next = cur;
    if (prev) prev->next = t;
    else      g_head = t;
    if (cur)  cur->prev = t;
    t->active = 1;
}

static void run_deferred_frees(void) {
    int n = g_defer_n;
    g_defer_n = 0;
    for (int i = 0; i < n; i++) {
        if (g_defer_fn[i] && g_defer_ptr[i])
            g_defer_fn[i](g_defer_ptr[i]);
        g_defer_fn[i] = NULL;
        g_defer_ptr[i] = NULL;
    }
}

static void on_wheel(uv_timer_t *handle) {
    QuicTimerFire fire[QUIC_TIMER_MAX_FIRE];
    int n = 0;
    uint64_t now;
    (void)handle;

    if (!g_loop || !g_head) return;
    now = uv_now(g_loop);
    g_dispatching = 1;

    /* 先全部出队并拷贝回调，再调用。回调里可能 Destruct/free 连接，
     * QuicTimer 嵌在连接对象上，不能在回调之后再解引用 due 指针。 */
    while (g_head && g_head->due_ms <= now && n < QUIC_TIMER_MAX_FIRE) {
        QuicTimer *t = g_head;
        uint64_t repeat = t->repeat_ms;
        fire[n].cb = t->cb;
        fire[n].user = t->user;
        n++;
        timer_unlink(t);
        if (repeat > 0) {
            t->cb = fire[n - 1].cb;
            t->user = fire[n - 1].user;
            t->repeat_ms = repeat;
            t->due_ms = now + repeat;
            timer_link_sorted(t);
        }
    }

    for (int i = 0; i < n; i++) {
        if (fire[i].cb) fire[i].cb(fire[i].user);
    }

    g_dispatching = 0;
    run_deferred_frees();
}

void quic_timer_wheel_init(uv_loop_t *loop) {
    if (g_inited) return;
    if (!loop) {
        LOG_ERROR("[quic-timer] wheel_init: loop is NULL");
        return;
    }
    g_loop = loop;
    memset(&g_uv, 0, sizeof(g_uv));
    uv_timer_init(loop, &g_uv);
    uv_timer_start(&g_uv, on_wheel, QUIC_TIMER_TICK_MS, QUIC_TIMER_TICK_MS);
    g_inited = 1;
    LOG_DEBUG("[quic-timer] wheel started, tick=%dms", QUIC_TIMER_TICK_MS);
}

void quic_timer_init(QuicTimer *t) {
    if (!t) return;
    memset(t, 0, sizeof(*t));
}

void quic_timer_start(QuicTimer *t, quic_timer_cb cb, void *user,
                      uint64_t timeout_ms, uint64_t repeat_ms) {
    if (!t || !g_loop) return;
    timer_unlink(t);
    t->cb = cb;
    t->user = user;
    t->repeat_ms = repeat_ms;
    t->due_ms = uv_now(g_loop) + timeout_ms;
    timer_link_sorted(t);
}

void quic_timer_stop(QuicTimer *t) {
    timer_unlink(t);
}

int quic_timer_is_active(const QuicTimer *t) {
    return t && t->active;
}

int quic_timer_in_dispatch(void) {
    return g_dispatching;
}

void quic_timer_defer_free(void *p, quic_timer_free_fn fn) {
    if (!p || !fn) return;
    if (!g_dispatching) {
        fn(p);
        return;
    }
    if (g_defer_n >= QUIC_TIMER_MAX_DEFER) {
        LOG_ERROR("[quic-timer] defer_free overflow, freeing immediately");
        fn(p);
        return;
    }
    g_defer_ptr[g_defer_n] = p;
    g_defer_fn[g_defer_n] = fn;
    g_defer_n++;
}
