#include "quic_cc.h"
#include "quic_common.h"
#include "logger.h"
#include <stdlib.h>
#include <string.h>
#include <uv.h>

/* ============================================
 * BBR v1 拥塞控制 (RFC 9002 + draft-cardwell-bbr)
 *
 * Model-based: cwnd = gain × max_bw × min_rtt
 * Does NOT react to isolated packet loss.
 *
 * 4-state: STARTUP → DRAIN → PROBE_BW ⇄ PROBE_RTT
 * ============================================ */

#define BBR_STARTUP    0
#define BBR_DRAIN      1
#define BBR_PROBE_BW   2
#define BBR_PROBE_RTT  3
#define BBR_MIN_RTT_WINDOW_MS  10000   /* WinMinRTT 窗口 */

#define BBR_BW_WINDOW      8     /* max bandwidth samples */
#define BBR_RTT_WINDOW_MS  10000 /* min rtt window */
#define BBR_MIN_RTT_US     3000  /* 3ms floor — hybrid loopback+proxy */
#define BBR_PROBE_RTT_DUR  200   /* ms, probe_rtt lasts this long */
/* ══ cwnd_gain 与 pacing_gain 是两个独立的增益（BBRv1）══
 *
 * 曾经的错误：PROBE_BW 下把 pacing 增益表当 cwnd 增益用，于是
 * cwnd = 1.0 × BDP（多数相位），交付速率恒等于 cwnd/RTT ≡ max_bw，
 * 测量只是在复述 cwnd，不携带链路信息（实测 max_bw ≈ cwnd/min_rtt）。
 *
 * 正确做法：cwnd_gain 在 PROBE_BW 恒为 2.0（留 2×BDP 余量，使
 * cwnd 不成为瓶颈），发送速率由 pacing_gain × BtlBw 决定。 */
#define BBR_STARTUP_GAIN_CWND    2.0
#define BBR_DRAIN_GAIN_CWND      0.5
#define BBR_PROBE_BW_GAIN_CWND   2.0   /* 常量，与相位无关 */
#define BBR_PROBE_RTT_GAIN_CWND  1.0

/* pacing gain per PROBE_BW phase (8-phase cycle) */
static const double bbr_pbw_gain[8] = {1.25, 0.75, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0};

/* STARTUP 的 pacing gain = 2/ln2，一輪可翻倍带宽 */
#define BBR_STARTUP_GAIN_PACING  2.89
#define BBR_DRAIN_GAIN_PACING    1.0

typedef struct {
    /* bandwidth */
    uint64_t bw_buf[BBR_BW_WINDOW];
    int      bw_idx;
    int      bw_count;
    uint64_t max_bw;            /* bytes/sec */

    /* RTT */
    uint64_t min_rtt_us;
    uint64_t min_rtt_stamp_ms;
    uint64_t last_probe_rtt_ms; /* 上次 PROBE_RTT 的时刻 (ms) */

    /* state machine */
    int      state;
    uint64_t state_start_ms;
    int      probe_bw_phase;    /* 0..7 */

    /* round tracking */
    uint64_t round_start_us;
    uint64_t round_bytes;
    uint64_t round_start_pn;    /* 本轮首包号：ACK 到它 = 本轮走完一圈 */
    uint64_t max_pn_sent;       /* 已发送的最大包号 */
    int      round_count;       /* startup rounds */
    uint64_t prev_max_bw;       /* bandwidth last round (startup exit) */

    /* delivery */
    uint64_t last_ack_us;

    /* cwnd */
    uint64_t cwnd_;
    uint64_t mss;               /* QUIC_MIN_PKT_SIZE */

    /* delivery rate estimation */
    uint64_t delivery_rate;     /* current sample */
    uint64_t delivery_rate_ts;
} quic_cc_bbr_t;

/* ── helpers ────────────────────────────────── */

static uint64_t bbr_now_us(void) {
    /* uv_now returns ms, convert to us. 0ms → 1us floor. */
    uint64_t ms = uv_now(uv_default_loop());
    return ms ? ms * 1000 : 1;
}

static void bbr_update_max_bw(quic_cc_bbr_t *bbr, uint64_t bw) {
    bbr->bw_buf[bbr->bw_idx] = bw;
    bbr->bw_idx = (bbr->bw_idx + 1) % BBR_BW_WINDOW;
    if (bbr->bw_count < BBR_BW_WINDOW) bbr->bw_count++;

    /* recompute max */
    bbr->max_bw = 0;
    for (int i = 0; i < bbr->bw_count; i++)
        if (bbr->bw_buf[i] > bbr->max_bw) bbr->max_bw = bbr->bw_buf[i];
}

/* cwnd 增益：只负责留出在途余量，不参与速率决策。
 * PROBE_BW 恒为 2.0 —— 曾经的 bbr_pbw_gain[phase] 是错的（见上方注释）。 */
static double bbr_cwnd_gain(const quic_cc_bbr_t *bbr) {
    switch (bbr->state) {
    case BBR_STARTUP:  return BBR_STARTUP_GAIN_CWND;
    case BBR_DRAIN:    return BBR_DRAIN_GAIN_CWND;
    case BBR_PROBE_BW: return BBR_PROBE_BW_GAIN_CWND;
    case BBR_PROBE_RTT:return BBR_PROBE_RTT_GAIN_CWND;
    default:           return 1.0;
    }
}

/* pacing 增益：决定发送速率 = gain × BtlBw。这才是相位增益的用武之地。
 * PROBE_BW 用它做周期探测（1.25 试探更高带宽 / 0.75 排空队列）。 */
static double bbr_pacing_gain(const quic_cc_bbr_t *bbr) {
    switch (bbr->state) {
    case BBR_STARTUP:  return BBR_STARTUP_GAIN_PACING;
    case BBR_DRAIN:    return BBR_DRAIN_GAIN_PACING;
    case BBR_PROBE_BW: return bbr_pbw_gain[bbr->probe_bw_phase];
    case BBR_PROBE_RTT:return 1.0;
    default:           return 1.0;
    }
}

/* update cwnd = cwnd_gain × BDP, with 4*MSS floor */
static void bbr_set_cwnd(quic_cc_bbr_t *bbr) {
    double gain = bbr_cwnd_gain(bbr);
    /* BDP = max_bw (B/s) × min_rtt (us) / 1e6 (B) */
    uint64_t bdp = (uint64_t)((double)bbr->max_bw
                    * (double)bbr->min_rtt_us / 1e6);
    uint64_t target = (uint64_t)(gain * (double)bdp);
    uint64_t floor = 4 * bbr->mss;
    if (target < floor) target = floor;
    bbr->cwnd_ = target;
}

static void bbr_enter_startup(quic_cc_bbr_t *bbr, uint64_t now_ms) {
    if (bbr->state == BBR_STARTUP) return;
    bbr->state = BBR_STARTUP;
    bbr->state_start_ms = now_ms;
    bbr->round_count = 0;
    bbr->prev_max_bw = 0;
    LOG_DEBUG("[cc-bbr] → STARTUP");
}

static void bbr_enter_drain(quic_cc_bbr_t *bbr, uint64_t now_ms) {
    bbr->state = BBR_DRAIN;
    bbr->state_start_ms = now_ms;
    bbr_set_cwnd(bbr);
    LOG_DEBUG("[cc-bbr] → DRAIN  cwnd=%llu", (unsigned long long)bbr->cwnd_);
}

static void bbr_enter_probe_bw(quic_cc_bbr_t *bbr) {
    bbr->state = BBR_PROBE_BW;
    bbr->probe_bw_phase = 0;
    bbr_set_cwnd(bbr);
    LOG_DEBUG("[cc-bbr] → PROBE_BW  max_bw=%llu min_rtt=%llu cwnd=%llu",
             (unsigned long long)bbr->max_bw,
             (unsigned long long)bbr->min_rtt_us,
             (unsigned long long)bbr->cwnd_);
}

static void bbr_enter_probe_rtt(quic_cc_bbr_t *bbr) {
    bbr->state = BBR_PROBE_RTT;
    bbr->state_start_ms = (uint64_t)(bbr_now_us() / 1000);
    bbr->last_probe_rtt_ms = bbr->state_start_ms;
    /* cwnd = 4 * MSS to drain queue and measure true min_rtt */
    bbr->cwnd_ = 4 * bbr->mss;
    LOG_DEBUG("[cc-bbr] → PROBE_RTT  cwnd=%llu", (unsigned long long)bbr->cwnd_);
}

static void bbr_handle_round_end(quic_cc_bbr_t *bbr, uint64_t now_us) {
    /* compute bandwidth for completed round */
    const uint64_t raw_elapsed_us = now_us - bbr->round_start_us;
    /* 只防 0 除与测量噪声，不假设 RTT 有下限。
     *
     * 原值 5000us（5ms）是在公网上防「本地回环算出 GB/s 级假值」加的，
     * 但它把回环的真实高带宽也一起压掉了：本地 RTT 常 <1ms，
     * elapsed 恒被截断成 5ms → bw = round_bytes/5ms → 恒定低报，
     * 而 cwnd = 2×bw×min_rtt 又反过来限制 round_bytes，
     * 形成不动点（实测锁死在 cwnd=4800 / max_bw≈565KB/s）。
     * 回环的高带宽不是噪声，是真值，不该被地板吃掉。 */
    uint64_t elapsed_us = raw_elapsed_us;
    if (elapsed_us < 100) elapsed_us = 100;
    /* bytes/sec = round_bytes * 1e6 / elapsed_us */
    uint64_t bw = (uint64_t)((double)bbr->round_bytes * 1e6
                             / (double)elapsed_us);
    bbr_update_max_bw(bbr, bw);

    /* 带 raw 值：一眼看出地板是否仍在生效（raw==elapsed 即未触发） */
    LOG_DEBUG("[cc-bbr] round end: %lluB/%lluus(raw=%lluus) = %llu B/s"
             "  max_bw=%llu state=%d cwnd=%llu",
             (unsigned long long)bbr->round_bytes,
             (unsigned long long)elapsed_us,
             (unsigned long long)raw_elapsed_us,
             (unsigned long long)bw,
             (unsigned long long)bbr->max_bw,
             bbr->state, (unsigned long long)bbr->cwnd_);

    /* state transitions */
    switch (bbr->state) {
    case BBR_STARTUP:
        bbr->round_count++;
        /* Exit STARTUP if bandwidth plateaued (3 rounds < 25% growth) */
        if (bbr->round_count >= 3) {
            if (bw <= bbr->prev_max_bw + bbr->prev_max_bw / 4) {
                bbr_enter_drain(bbr, (uint64_t)(now_us / 1000));
                break;
            }
        }
        bbr->prev_max_bw = bw;
        bbr_set_cwnd(bbr);
        break;

    case BBR_DRAIN:
        /* Exit DRAIN when inflight drops to BDP */
        bbr_enter_probe_bw(bbr);
        break;

    case BBR_PROBE_BW:
        /* advance phase */
        bbr->probe_bw_phase = (bbr->probe_bw_phase + 1) & 7;
        bbr_set_cwnd(bbr);

        /* Check PROBE_RTT: 每 10s 重探一次真实地板（对齐 WinMinRTT 窗口）*/
        if ((now_us / 1000) - bbr->last_probe_rtt_ms >= BBR_RTT_WINDOW_MS) {
            LOG_INFO("[cc-bbr] PROBE_RTT triggered: %llu ms since last probe",
                     (unsigned long long)((now_us / 1000) - bbr->last_probe_rtt_ms));
            bbr_enter_probe_rtt(bbr);
        }
        break;

    case BBR_PROBE_RTT:
        /* Exit PROBE_RTT after 200ms */
        if ((now_us / 1000) - bbr->state_start_ms >= BBR_PROBE_RTT_DUR) {
            bbr_enter_probe_bw(bbr);
        }
        break;
    }

    /* start new round */
    bbr->round_start_us = now_us;
    bbr->round_bytes = 0;
    bbr->round_start_pn = bbr->max_pn_sent + 1;   /* 下一轮的首包号 */
}

/* ── 回调 ────────────────────────────────── */

static void bbr_on_packet_sent(struct quic_cc *cc, uint64_t pn,
                               uint64_t bytes, uint64_t now_ms) {
    quic_cc_bbr_t *bbr = (quic_cc_bbr_t*)cc->priv;
    (void)bytes; (void)now_ms;
    if (pn > bbr->max_pn_sent) bbr->max_pn_sent = pn;
}

static void bbr_on_packet_acked(struct quic_cc *cc, uint64_t pn,
                                uint64_t bytes, uint64_t rtt_us,
                                uint64_t now_ms) {
    quic_cc_bbr_t *bbr = (quic_cc_bbr_t*)cc->priv;
    uint64_t now_us = bbr_now_us();

    /* update min_rtt — BBR only lowers, tracking the path floor */
    if (rtt_us > 0 && rtt_us < 10000000) {
        uint64_t r = rtt_us < BBR_MIN_RTT_US ? BBR_MIN_RTT_US : rtt_us;
        if (now_ms - bbr->min_rtt_stamp_ms > BBR_MIN_RTT_WINDOW_MS || bbr->min_rtt_us == 0) {
            bbr->min_rtt_us = r;      // ← 直接用本次采样
            bbr->min_rtt_stamp_ms = now_ms; 
        } else if (r < bbr->min_rtt_us) {
            bbr->min_rtt_us = r;
        }
    }

    /* accumulate round bytes & detect round end */
    if (bbr->round_bytes == 0)
        bbr->round_start_us = now_us;
    bbr->round_bytes += bytes;

    /* STARTUP: aggressive cwnd growth per ACK (≈slow start) */
    if (bbr->state == BBR_STARTUP) {
        bbr->cwnd_ += bytes;
        if ((int)bbr->round_count < 1) {
            LOG_DEBUG("[cc-bbr] STARTUP: cwnd=%llu (+%llu) rtt=%llu bw=%llu, pn=%llu",
                     (unsigned long long)bbr->cwnd_, (unsigned long long)bytes,
                     (unsigned long long)rtt_us,
                     (unsigned long long)bbr->max_bw, pn);
        }
    }

    /* In BBR, a round ends when we ACK a packet sent after the round start. */
    if (pn >= bbr->round_start_pn) {
        bbr_handle_round_end(bbr, now_us);
    }

    bbr->last_ack_us = now_us;
}

static void bbr_on_congestion_event(struct quic_cc *cc, uint64_t now_ms) {
    (void)cc; (void)now_ms;
    /* BBR does NOT reduce cwnd on isolated loss.
     * Random packet loss is not a congestion signal. */
    LOG_DEBUG("[cc-bbr] congestion event — IGNORED (not BW signal)");
}

static void bbr_on_persistent_congestion(struct quic_cc *cc, uint64_t now_ms) {
    quic_cc_bbr_t *bbr = (quic_cc_bbr_t*)cc->priv;

    /* RESET: path change — 但不要把 max_bw 打到几乎为 0，否则 WAN 上很难爬升 */
    bbr->cwnd_ = 10 * bbr->mss;
    if (bbr->max_bw > bbr->mss * 200)
        bbr->max_bw = bbr->max_bw / 4;
    if (bbr->max_bw < bbr->mss * 100)
        bbr->max_bw = bbr->mss * 100;
    memset(bbr->bw_buf, 0, sizeof(bbr->bw_buf));
    bbr->bw_idx = bbr->bw_count = 0;
    bbr_enter_startup(bbr, now_ms);

    LOG_DEBUG("[cc-bbr] persistent congestion — RESET to startup cwnd=%llu max_bw=%llu",
             (unsigned long long)bbr->cwnd_, (unsigned long long)bbr->max_bw);
}

static uint64_t bbr_get_cwnd(const struct quic_cc *cc) {
    const quic_cc_bbr_t *bbr_const = (const quic_cc_bbr_t*)cc->priv;
    quic_cc_bbr_t *bbr = (quic_cc_bbr_t*)bbr_const;   /* 需要改下 const */

    if (!bbr) return 0;

    /* 这里被 quic_cc_can_send 每次调用 —— 不依赖 ACK，
     * PROBE_RTT 超时能在这里强制退出，避免死锁。 */
    if (bbr->state == BBR_PROBE_RTT) {
        uint64_t now_ms = bbr_now_us() / 1000;
        if (now_ms - bbr->state_start_ms >= BBR_PROBE_RTT_DUR) {
            LOG_INFO("[cc-bbr] PROBE_RTT timeout (%llums), force → PROBE_BW",
                     (unsigned long long)(now_ms - bbr->state_start_ms));
            bbr_enter_probe_bw(bbr);
        }
    }
    return bbr->cwnd_;
}

static void bbr_get_info(const struct quic_cc *cc, struct quic_cc_info *out) {
    const quic_cc_bbr_t *bbr = (const quic_cc_bbr_t*)cc->priv;
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->cwnd = bbr->cwnd_;
    out->max_bw_bps = bbr->max_bw;
    out->algo = QUIC_CC_ALGO_BBR;
    out->state = bbr->state;
    out->min_rtt_us = bbr->min_rtt_us;
    /* 发送速率 = pacing_gain × BtlBw。max_bw 尚未建立时保持 0，
     * 发送路径据此判定「pacing 未就绪」而放行。 */
    if (bbr->max_bw > 0) {
        double pg = bbr_pacing_gain(bbr);
        double rate = pg * (double)bbr->max_bw;
        if (rate < 0.0) rate = 0.0;

        /* ── pacing 下限：不低于 cwnd 地板对应的速率 ──
         *
         * 没有这道下限时，max_bw 会被拖到远低于应用需求的水平
         * （实测下行 116KB/s vs 需求 500KB/s）。后果是 pacing 自己
         * 成为瓶颈：单个大 object（关键帧可达 81~190KB）在低速率下
         * 需要数百 ms 才能发完，chunk 迟迟无法完成而被 PTO 判为
         * 「该重传」，反复 20 次后连接被误判 dead。
         *
         * 下限取 4×MSS/min_rtt —— 即 cwnd 地板所对应的带宽。
         * 物理含义：只要 cwnd 允许在途 4 个包，发送速率就不该低于
         * 「每个 RTT 送完这 4 个包」。这是从现有参数推导的，不是拍脑袋值。
         *
         * 注意这只抬 pace_rate，不动 max_bw 本身 —— max_bw 仍可继续
         * 衰减，只是不再把发送速率一起拖下去。
         *
         * min_rtt 未测量时（=0）必须跳过下限，不能用 1us 兜底：
         * 那样会算出 4×MSS/1us ≈ 4.8GB/s 的"下限"，实际等价于禁用 pacing
         * ——而 STARTUP 恰恰是最需要 pacing 抑制突发的阶段。
         * 此阶段 pacing_rate 本就由初始 max_bw 推得，无需额外保护。 */
        if (bbr->min_rtt_us > 0) {
            const double floor_rate =
                (4.0 * (double)bbr->mss * 1e6) / (double)bbr->min_rtt_us;
            if (rate < floor_rate) {
                /* 诊断：下限生效说明 max_bw 被估到了远低于「cwnd 地板对应
                 * 速率」的水平。若这条频繁出现，问题在测量侧（ACK 稀疏 /
                 * 窗口自锁），而不是发送侧。1 秒限流，避免每包一条。 */
                static uint64_t last_floor_log_ms = 0;
                const uint64_t now_ms = bbr_now_us() / 1000;
                if (now_ms - last_floor_log_ms >= 1000) {
                    last_floor_log_ms = now_ms;
                    LOG_INFO("[cc-bbr] pacing floor applied: rate=%.0f -> %.0f B/s "
                             "(max_bw=%llu pacing_gain=%.2f min_rtt=%lluus state=%d)",
                             rate, floor_rate,
                             (unsigned long long)bbr->max_bw, pg,
                             (unsigned long long)bbr->min_rtt_us, bbr->state);
                }
                rate = floor_rate;
            }
        }

        if (rate > 1e12) rate = 1e12;   /* 防溢出到 uint64 之外 */
        out->pacing_rate_bps = (uint64_t)rate;
    }
}

static void bbr_destroy(struct quic_cc *cc) {
    free(cc->priv);
    free(cc);
}

/* ── 工厂 ────────────────────────────────── */

static const struct quic_cc_ops bbr_ops = {
    .on_packet_sent           = bbr_on_packet_sent,
    .on_packet_acked          = bbr_on_packet_acked,
    .on_congestion_event      = bbr_on_congestion_event,
    .on_persistent_congestion = bbr_on_persistent_congestion,
    .get_cwnd                 = bbr_get_cwnd,
    .get_info                 = bbr_get_info,
    .destroy                  = bbr_destroy,
};

struct quic_cc *quic_cc_bbr_create(void) {
    struct quic_cc *cc = (struct quic_cc*)calloc(1, sizeof(*cc));
    if (!cc) return NULL;

    quic_cc_bbr_t *bbr = (quic_cc_bbr_t*)calloc(1, sizeof(*bbr));
    if (!bbr) { free(cc); return NULL; }

    bbr->mss          = QUIC_MIN_PKT_SIZE;
    bbr->cwnd_        = 32 * bbr->mss;  /* ~38KB initial window */
    bbr->min_rtt_us   = 0;
    bbr->min_rtt_stamp_ms = 0;
    bbr->last_probe_rtt_ms = bbr_now_us() / 1000;
    bbr->max_bw       = bbr->mss * 100; /* 120KB/s minimal */

    bbr->round_start_pn = 0;                  /* 显式写出，calloc 已置零 */
    bbr->max_pn_sent    = 0;
    bbr->round_start_us = bbr_now_us();       /* 避免第一轮 elapsed 从 0 起算 */

    cc->ops  = &bbr_ops;
    cc->priv = bbr;

    LOG_INFO("[cc-bbr] created: cwnd=%llu mss=%llu",
             (unsigned long long)bbr->cwnd_, (unsigned long long)bbr->mss);
    return cc;
}
