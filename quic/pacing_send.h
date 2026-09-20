#ifndef QUIC_PACING_SEND_H
#define QUIC_PACING_SEND_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================
 * 令牌桶 pacing
 *
 * 解决的是「cwnd 既是限速器又是被测量对象」的循环论证：
 * 没有 pacing 时，交付速率 ≡ cwnd/RTT，于是 BBR 测出的 max_bw
 * 只是在复述 cwnd，不携带链路信息（实测 max_bw ≈ cwnd/min_rtt）。
 * 引入 pacing 后发送速率由 pacing_rate 决定，测量才有意义。
 *
 * 单位：字节 / 秒，时间用纳秒（uv_hrtime，单调）。
 * 不用 uv_now() —— 它是毫秒缓存值，做不了亚毫秒计量。
 *
 * ══ 设计取舍 ══
 * 定时器精度为 QUIC_TIMER_TICK_MS（1ms）。本模块只做「准入判定」，
 * 不自行调度发送 —— 调用方应在一次 tick 内循环调用 acquire，
 * 直到令牌耗尽，让令牌桶成为唯一的限速器。
 * （若每次只发一个包就退避，速率会被限到 payload/tick，远低于目标。）
 * ============================================ */

/* 令牌上限的硬上限，防止高码率下 burst 过大而失去平滑意义 */
#define PACING_MAX_BURST_CAP   (64 * 1024)

/* burst 下限：低于 2 个包时即使速率很低也要能推进，
 * 否则一个包都发不出去。1200 与 QUIC_MIN_PKT_SIZE 对齐，
 * 这里写死避免头文件互相依赖。 */
#define PACING_MIN_BURST       (2 * 1200)

/* 初始 burst：pacing_rate 尚未建立（max_bw 还没算出来）时使用。
 * 此时 rate_bps == 0、pacing 整体禁用，该值只影响速率刚建立的一瞬。 */
#define PACING_INIT_BURST      PACING_MIN_BURST

typedef struct PacingSend {
    uint64_t rate_bps;        /* 当前 pacing 速率（字节/秒）；0 = 禁用 */
    double   tokens;          /* 令牌余额（字节）。double 避免低速率下的取整损耗 */
    uint64_t last_refill_ns;  /* 上次补充时刻；0 = 未锚定 */
    uint64_t max_burst;       /* 令牌上限，防空闲后突发 */
} PacingSend;

/* 初始化。max_burst_bytes=0 时取 PACING_INIT_BURST。
 * rate_bps=0 表示初始禁用 pacing（放行一切）。 */
void pacing_send_init(PacingSend *p, uint64_t rate_bps,
                      uint64_t max_burst_bytes);

/* 更新速率。会按新速率重算 max_burst，并重置时间锚点 ——
 * 否则会拿新速率去乘「按旧速率计时」的 delta_ns，令牌算错。
 * rate_bps=0 表示禁用（放行一切）并清零令牌。 */
void pacing_send_set_rate(PacingSend *p, uint64_t rate_bps);

/* 补充令牌。now_ns 传 uv_hrtime()。可重复调用（幂等推进）。 */
void pacing_send_refill(PacingSend *p, uint64_t now_ns);

/* 准入判定：
 *   返回 1 = 可以发，且已扣除 bytes 个令牌
 *   返回 0 = 令牌不足，不扣除，调用方应稍后重试
 * rate_bps == 0 时恒返回 1（pacing 禁用）。 */
int pacing_send_acquire(PacingSend *p, uint64_t bytes, uint64_t now_ns);

/* 下一次发送 bytes 字节所需等待的纳秒数。0 表示现在即可发。
 * 供调用方估算退避间隔用。 */
uint64_t pacing_send_wait_ns(const PacingSend *p, uint64_t bytes,
                             uint64_t now_ns);

/* 当前速率（0 = 禁用）。供日志/统计用 */
uint64_t pacing_send_rate(const PacingSend *p);

#ifdef __cplusplus
}
#endif

#endif /* QUIC_PACING_SEND_H */
