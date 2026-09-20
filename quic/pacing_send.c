#include "pacing_send.h"

#include <uv.h>

/* burst = 10ms 对应的发送量：够吸收 ACK 到达的时间抖动，
 * 又不至于攒出一个明显突发。低码率下由 PACING_MIN_BURST 兜底，
 * 高码率下由 PACING_MAX_BURST_CAP 封顶。 */
#define PACING_BURST_WINDOW_US   10000
#define NS_PER_SEC               1000000000ULL

static uint64_t pacing_calc_burst(uint64_t rate_bps) {
    /* burst = 速率 × 窗口时长。用 double 避免低速率下的整数截断。 */
    uint64_t burst = (uint64_t)((double)rate_bps *
                                (double)PACING_BURST_WINDOW_US / 1e6);
    if (burst < PACING_MIN_BURST) burst = PACING_MIN_BURST;
    if (burst > PACING_MAX_BURST_CAP) burst = PACING_MAX_BURST_CAP;
    return burst;
}

void pacing_send_init(PacingSend *p, uint64_t rate_bps,
                      uint64_t max_burst_bytes) {
    if (!p) return;
    p->rate_bps = rate_bps;
    p->tokens = 0.0;
    p->last_refill_ns = 0;
    p->max_burst = max_burst_bytes ? max_burst_bytes
                                   : (rate_bps ? pacing_calc_burst(rate_bps)
                                               : PACING_INIT_BURST);
}

void pacing_send_set_rate(PacingSend *p, uint64_t rate_bps) {
    if (!p) return;
    if (p->rate_bps == rate_bps) return;

    p->rate_bps = rate_bps;
    /* 换档时重置时间锚点：否则会用新速率去乘「按旧速率计时」的
     * delta_ns，算出的令牌是错的。代价是丢弃这一小段未结算的量。 */
    p->last_refill_ns = 0;
    if (rate_bps == 0) {
        /* 禁用时清零，避免重新启用时一次补齐攒下的令牌 */
        p->tokens = 0.0;
        p->max_burst = PACING_INIT_BURST;
    } else {
        p->max_burst = pacing_calc_burst(rate_bps);
        if (p->tokens > (double)p->max_burst) {
            p->tokens = (double)p->max_burst;
        }
    }
}

void pacing_send_refill(PacingSend *p, uint64_t now_ns) {
    if (!p || p->rate_bps == 0) return;

    if (p->last_refill_ns == 0) {
        /* 首次或刚换档：只锚定，不补令牌 */
        p->last_refill_ns = now_ns;
        return;
    }
    /* 时间回退保护：无符号相减会得到天文数字，令令牌暴增 */
    if (now_ns <= p->last_refill_ns) return;

    const uint64_t delta_ns = now_ns - p->last_refill_ns;
    /* 用 double 累加：低速率下短间隔只能攒出小数个字节
     * （500KB/s × 1ms = 500 字节尚可，但 5KB/s × 1ms = 5 字节
     * 在更短的 delta 下会截断成 0）。整数会永远攒不够 → 停滞。 */
    p->tokens += (double)p->rate_bps * (double)delta_ns / (double)NS_PER_SEC;

    /* 上限钳制是防突发的关键：没有它，空闲 1 秒会攒够 1 秒的
     * 令牌并一次性打出，比不做 pacing 更糟（更大的突发）。 */
    if (p->tokens > (double)p->max_burst) p->tokens = (double)p->max_burst;

    p->last_refill_ns = now_ns;
}

int pacing_send_acquire(PacingSend *p, uint64_t bytes, uint64_t now_ns) {
    if (!p) return 1;
    if (p->rate_bps == 0) return 1;   /* 禁用：放行 */

    pacing_send_refill(p, now_ns);

    if (p->tokens >= (double)bytes) {
        p->tokens -= (double)bytes;
        return 1;
    }
    return 0;
}

uint64_t pacing_send_wait_ns(const PacingSend *p, uint64_t bytes,
                             uint64_t now_ns) {
    if (!p || p->rate_bps == 0) return 0;

    /* p 是 const，这里不 refill（查询函数不该改状态），
     * 改为把令牌向前推算到 now_ns —— 否则会基于过期余额算出偏大的等待值。 */
    double avail = p->tokens;
    if (p->last_refill_ns != 0 && now_ns > p->last_refill_ns) {
        avail += (double)p->rate_bps *
                 (double)(now_ns - p->last_refill_ns) / (double)NS_PER_SEC;
    }
    if (avail > (double)p->max_burst) avail = (double)p->max_burst;

    if (avail >= (double)bytes) return 0;

    const double need = (double)bytes - avail;
    const double wait = need / (double)p->rate_bps * (double)NS_PER_SEC;
    if (wait <= 0.0) return 0;
    if (wait > 1e12) return 1000000000000ULL;   /* 上限 1000s，防溢出 */
    return (uint64_t)wait;
}

uint64_t pacing_send_rate(const PacingSend *p) {
    return p ? p->rate_bps : 0;
}
