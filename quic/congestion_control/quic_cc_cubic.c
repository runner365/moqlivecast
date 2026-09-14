#include "quic_cc.h"
#include "quic_common.h"
#include "logger.h"
#include <stdlib.h>
#include <string.h>

/* ============================================
 * CUBIC 拥塞控制 (RFC 8312)
 *
 * cwnd 决策：
 *   - 慢启动：cwnd += acked，直到 ssthresh
 *   - 拥塞避免：cwnd 按三次函数增长
 *       W_cubic(t) = C*(t-K)^3 + W_max
 *       K = cbrt(W_max*(1-beta)/C)
 *       C = 0.4, beta = 0.7
 *   - TCP-friendly 区域：与 Reno 估计取较大值，保证和 Reno 公平竞争
 *   - 拥塞事件：cwnd *= beta，(可选)快收敛
 *   - 持续拥塞：重置到初始窗口
 *
 * 这是为「替换 BBR 做对照实验」而写的，行为对齐标准 CUBIC。
 * ============================================ */

#define CUBIC_C       0.4
#define CUBIC_BETA    0.7    /* 乘法减小因子；cwnd *= 0.7 */
#define CUBIC_INIT_W  (10 * QUIC_MIN_PKT_SIZE)   /* RFC 9002 初始窗口 ~10*MSS */

typedef struct {
    uint64_t cwnd_;
    uint64_t ssthresh_;
    uint64_t w_max_;          /* 上次降窗前的窗口（cubic 原点） */
    uint64_t epoch_start_ms_; /* 上次窗口减小的时刻 */
    uint64_t bytes_acked_;    /* 拥塞避免：累积已确认字节，满一个 cwnd 重算一次 */
    int      in_slow_start_;
    uint64_t min_rtt_us_;     /* TCP-friendly 估计用 */
} quic_cc_cubic_t;

/* 立方根（牛顿迭代）——避免依赖 libm 的 cbrt() */
static double cubic_cbrt(double x) {
    if (x <= 0.0) return 0.0;
    double r = x > 1.0 ? x : 1.0;
    for (int i = 0; i < 60; i++) {
        double r2 = r * r;
        double nr = (2.0 * r + x / r2) / 3.0;
        if (nr > r - 1e-9 && nr < r + 1e-9) { r = nr; break; }
        r = nr;
    }
    return r;
}

/* ── 回调 ────────────────────────────────── */

static void cubic_on_packet_sent(struct quic_cc *cc, uint64_t pn,
                                 uint64_t bytes, uint64_t now_ms) {
    (void)cc; (void)pn; (void)bytes; (void)now_ms;
}

static void cubic_on_packet_acked(struct quic_cc *cc, uint64_t pn,
                                  uint64_t bytes, uint64_t rtt_us,
                                  uint64_t now_ms) {
    (void)pn;

    quic_cc_cubic_t *c = (quic_cc_cubic_t*)cc->priv;
    if (bytes == 0) return;

    /* 采集 RTT（min 用于 TCP-friendly 估计） */
    if (rtt_us > 0 && rtt_us < 10000000ULL) {
        if (c->min_rtt_us_ == 0 || rtt_us < c->min_rtt_us_)
            c->min_rtt_us_ = rtt_us;
    }

    /* ── 慢启动 ── */
    if (c->in_slow_start_) {
        c->cwnd_ += bytes;
        if (c->cwnd_ >= c->ssthresh_) {
            c->in_slow_start_ = 0;
            c->epoch_start_ms_ = now_ms;   /* 进入拥塞避免 */
            LOG_DEBUG("[cc-cubic] -> CA cwnd=%llu ssthresh=%llu",
                      (unsigned long long)c->cwnd_,
                      (unsigned long long)c->ssthresh_);
        }
        return;
    }

    /* ── 拥塞避免：每确认一个 cwnd 的数据重算一次 ── */
    c->bytes_acked_ += bytes;
    if (c->bytes_acked_ < c->cwnd_) return;
    c->bytes_acked_ = 0;

    double t = (double)(now_ms - c->epoch_start_ms_) / 1000.0;
    if (t < 0) t = 0;

    /* cubic 目标窗口 */
    double k = cubic_cbrt((double)c->w_max_ * (1.0 - CUBIC_BETA) / CUBIC_C);
    double diff = t - k;
    double w_cubic = CUBIC_C * diff * diff * diff + (double)c->w_max_;

    /* TCP-friendly 估计（保证与 Reno 公平）；公式单位换算回字节 */
    double rtt_s = (c->min_rtt_us_ ? (double)c->min_rtt_us_ : 100000.0) / 1e6;
    if (rtt_s <= 0) rtt_s = 0.1;
    double w_est = (double)c->w_max_ * CUBIC_BETA
                 + (3.0 * (1.0 - CUBIC_BETA) / (1.0 + CUBIC_BETA))
                   * (t / rtt_s) * (double)QUIC_MIN_PKT_SIZE;

    double target = w_cubic > w_est ? w_cubic : w_est;

    /* 只增不减（减窗由拥塞事件负责）；低于下限保护 */
    if (target > (double)c->cwnd_) {
        c->cwnd_ = (uint64_t)target;
    }

    LOG_DEBUG("[cc-cubic] CA t=%.3fs w_cubic=%.0f w_est=%.0f cwnd=%llu",
              t, w_cubic, w_est, (unsigned long long)c->cwnd_);
}

static void cubic_on_congestion_event(struct quic_cc *cc, uint64_t now_ms) {
    quic_cc_cubic_t *c = (quic_cc_cubic_t*)cc->priv;

    /* 快收敛：如果当前窗口已小于上次降窗前的窗口，进一步缩 W_max */
    if (c->cwnd_ < c->w_max_) {
        c->w_max_ = (uint64_t)((double)c->cwnd_ * (1.0 + CUBIC_BETA) / 2.0);
    } else {
        c->w_max_ = c->cwnd_;
    }

    c->ssthresh_ = (uint64_t)((double)c->cwnd_ * CUBIC_BETA);
    if (c->ssthresh_ < 2 * QUIC_MIN_PKT_SIZE)
        c->ssthresh_ = 2 * QUIC_MIN_PKT_SIZE;

    c->cwnd_ = c->ssthresh_;
    c->bytes_acked_ = 0;
    c->in_slow_start_ = 0;         /* 拥塞后直接进拥塞避免 */
    c->epoch_start_ms_ = now_ms;

    LOG_DEBUG("[cc-cubic] congestion: cwnd=%llu ssthresh=%llu w_max=%llu",
              (unsigned long long)c->cwnd_,
              (unsigned long long)c->ssthresh_,
              (unsigned long long)c->w_max_);
}

static void cubic_on_persistent_congestion(struct quic_cc *cc, uint64_t now_ms) {
    (void)now_ms;
    quic_cc_cubic_t *c = (quic_cc_cubic_t*)cc->priv;

    /* RFC 9002 §6.2：重置到最小窗口 */
    c->cwnd_ = 2 * QUIC_MIN_PKT_SIZE;
    c->ssthresh_ = 2 * QUIC_MIN_PKT_SIZE;
    c->w_max_ = 0;
    c->bytes_acked_ = 0;
    c->in_slow_start_ = 1;

    LOG_DEBUG("[cc-cubic] persistent congestion: cwnd reset to %llu",
             (unsigned long long)c->cwnd_);
}

static uint64_t cubic_get_cwnd(const struct quic_cc *cc) {
    const quic_cc_cubic_t *c = (const quic_cc_cubic_t*)cc->priv;
    return c->cwnd_;
}

static void cubic_get_info(const struct quic_cc *cc, struct quic_cc_info *out) {
    const quic_cc_cubic_t *c = (const quic_cc_cubic_t*)cc->priv;
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->cwnd = c->cwnd_;
    out->ssthresh = c->ssthresh_;
    out->max_bw_bps = 0;
    out->algo = QUIC_CC_ALGO_CUBIC;
    out->state = c->in_slow_start_ ? 0 : 1;   /* 0=slow start, 1=CA */
    out->min_rtt_us = c->min_rtt_us_;
}

static void cubic_destroy(struct quic_cc *cc) {
    free(cc->priv);
    free(cc);
}

/* ── 工厂 ────────────────────────────────── */

static const struct quic_cc_ops cubic_ops = {
    .on_packet_sent             = cubic_on_packet_sent,
    .on_packet_acked            = cubic_on_packet_acked,
    .on_congestion_event        = cubic_on_congestion_event,
    .on_persistent_congestion   = cubic_on_persistent_congestion,
    .get_cwnd                   = cubic_get_cwnd,
    .get_info                   = cubic_get_info,
    .destroy                    = cubic_destroy,
};

struct quic_cc *quic_cc_cubic_create(void) {
    struct quic_cc *cc = (struct quic_cc*)calloc(1, sizeof(*cc));
    if (!cc) return NULL;

    quic_cc_cubic_t *c = (quic_cc_cubic_t*)calloc(1, sizeof(*c));
    if (!c) { free(cc); return NULL; }

    c->cwnd_          = CUBIC_INIT_W;
    c->ssthresh_      = UINT64_MAX;   /* 慢启动直到第一次丢包/事件 */
    c->w_max_         = 0;
    c->epoch_start_ms_= 0;
    c->bytes_acked_   = 0;
    c->in_slow_start_ = 1;
    c->min_rtt_us_    = 0;

    cc->ops  = &cubic_ops;
    cc->priv = c;

    LOG_DEBUG("[cc-cubic] created: cwnd=%llu (init %d*MSS)",
             (unsigned long long)c->cwnd_, CUBIC_INIT_W / QUIC_MIN_PKT_SIZE);
    return cc;
}
