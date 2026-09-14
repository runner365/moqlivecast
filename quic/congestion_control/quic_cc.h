#ifndef QUIC_CC_H
#define QUIC_CC_H

#include "quic_common.h"
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================
 * 拥塞控制接口 (CC — Congestion Control)
 *
 * 通用层 (quic_recovery) 负责丢包检测 / PTO / RTT /
 * bytes_in_flight 记账，CC 层只负责 cwnd 决策。
 * ============================================ */

#define QUIC_CC_ALGO_UNKNOWN  0
#define QUIC_CC_ALGO_NEWRENO  1
#define QUIC_CC_ALGO_BBR      2
#define QUIC_CC_ALGO_CUBIC    3

/* 不透明类型 */
struct quic_cc;

struct quic_cc_info {
    uint64_t cwnd;         /* bytes */
    uint64_t ssthresh;     /* bytes; 0 if unused */
    uint64_t max_bw_bps;   /* BBR delivery estimate, bytes/s; 0 if unused */
    int      algo;         /* QUIC_CC_ALGO_* */
    int      state;        /* algo-specific (BBR phase / NewReno SS vs CA) */
    uint64_t min_rtt_us;   /* BBR delivery estimate, us; 0 if unused */
};

/* ── CC 算法必须实现的回调 ────────────────── */
struct quic_cc_ops {
    /* 发包后通知 — CC 不需要做任何事 */
    void (*on_packet_sent)(struct quic_cc *cc, uint64_t pn,
                           uint64_t bytes, uint64_t now_ms);

    /* 包被确认后通知 — CC 可增大 cwnd */
    void (*on_packet_acked)(struct quic_cc *cc, uint64_t pn,
                            uint64_t bytes, uint64_t rtt_us,
                            uint64_t now_ms);

    /* 包被判定丢失 — CC 可减小 cwnd */
    void (*on_congestion_event)(struct quic_cc *cc, uint64_t now_ms);

    /* 持续拥塞 (RFC 9002 §6.2) — pto_count ≥ 3，cwnd 重置到 min */
    void (*on_persistent_congestion)(struct quic_cc *cc, uint64_t now_ms);

    /* 查询当前 cwnd (字节) */
    uint64_t (*get_cwnd)(const struct quic_cc *cc);

    /* 可选：填充算法快照。未实现则为 NULL */
    void (*get_info)(const struct quic_cc *cc, struct quic_cc_info *out);

    /* 释放 CC 算法私有数据 */
    void (*destroy)(struct quic_cc *cc);
};

/* ── CC 对象 ─────────────────────────────── */
struct quic_cc {
    const struct quic_cc_ops *ops;
    void                     *priv;  /* 算法私有数据 */
};

/* ── 工厂函数 ────────────────────────────── */

/* 创建 NewReno (RFC 9002) */
struct quic_cc *quic_cc_newreno_create(void);

/* 创建 BBR (Bottleneck Bandwidth and Round-trip) */
struct quic_cc *quic_cc_bbr_create(void);

/* 创建 CUBIC (RFC 8312) */
struct quic_cc *quic_cc_cubic_create(void);

/* ── 便捷包装 ────────────────────────────── */

/* 判断当前 cwnd 是否允许发送 (通用层使用) */
static inline int quic_cc_can_send(const struct quic_cc *cc,
                                   uint64_t bytes_in_flight) {
    uint64_t cwnd = cc->ops->get_cwnd(cc);
    if (bytes_in_flight < cwnd) return 1;
    /* 降窗（如 BBR persistent congestion）后 bif 可能短暂高于 cwnd。
     * 严格 < 会饿死发送、ACK 时钟停摆。允许超出至多 1×MSS 再发一包。 */
    return bytes_in_flight < cwnd + QUIC_MIN_PKT_SIZE;
}

#ifdef __cplusplus
}
#endif

#endif /* QUIC_CC_H */
