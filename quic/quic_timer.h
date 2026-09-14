#ifndef QUIC_TIMER_H
#define QUIC_TIMER_H

#include <uv.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 进程内一个 5ms uv_timer 心跳；业务定时器用登记表，连接析构只需出队。
 * 精度为心跳粒度，timeout < 5ms 会在下一次 tick 触发。 */

#define QUIC_TIMER_TICK_MS 5

typedef void (*quic_timer_cb)(void *user);
typedef void (*quic_timer_free_fn)(void *p);

typedef struct QuicTimer {
    uint64_t      due_ms;
    uint64_t      repeat_ms;   /* 0 = 一次性 */
    quic_timer_cb cb;
    void         *user;
    int           active;
    struct QuicTimer *next;
    struct QuicTimer *prev;
} QuicTimer;

/* 幂等。listener / 首个 connection 创建时调用。 */
void quic_timer_wheel_init(uv_loop_t *loop);

void quic_timer_init(QuicTimer *t);
void quic_timer_start(QuicTimer *t, quic_timer_cb cb, void *user,
                      uint64_t timeout_ms, uint64_t repeat_ms);
void quic_timer_stop(QuicTimer *t);
int  quic_timer_is_active(const QuicTimer *t);

/* 心跳回调进行中：此时不能 free 仍可能被本轮 due 列表引用的对象。 */
int  quic_timer_in_dispatch(void);
/* 若正在 dispatch 则排队到本轮回调结束后再 fn(p)，否则立即 fn(p)。 */
void quic_timer_defer_free(void *p, quic_timer_free_fn fn);

#ifdef __cplusplus
}
#endif

#endif /* QUIC_TIMER_H */
