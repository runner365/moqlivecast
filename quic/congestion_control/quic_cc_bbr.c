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

#define BBR_BW_WINDOW      8     /* max bandwidth samples */
#define BBR_RTT_WINDOW_MS  10000 /* min rtt window */
#define BBR_MIN_RTT_US     3000  /* 3ms floor — hybrid loopback+proxy */
#define BBR_PROBE_RTT_DUR  200   /* ms, probe_rtt lasts this long */
#define BBR_STARTUP_GAIN_CWND    2.0
#define BBR_DRAIN_GAIN_CWND      0.5

/* pacing gain per PROBE_BW phase (8-phase cycle) */
static const double bbr_pbw_gain[8] = {1.25, 0.75, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0};

typedef struct {
    /* bandwidth */
    uint64_t bw_buf[BBR_BW_WINDOW];
    int      bw_idx;
    int      bw_count;
    uint64_t max_bw;            /* bytes/sec */

    /* RTT */
    uint64_t min_rtt_us;
    uint64_t rtt_probe_us;      /* last min_rtt update timestamp (us) */

    /* state machine */
    int      state;
    uint64_t state_start_ms;
    int      probe_bw_phase;    /* 0..7 */

    /* round tracking */
    uint64_t round_start_us;
    uint64_t round_bytes;
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

static double bbr_cwnd_gain(const quic_cc_bbr_t *bbr) {
    switch (bbr->state) {
    case BBR_STARTUP:  return BBR_STARTUP_GAIN_CWND;
    case BBR_DRAIN:    return BBR_DRAIN_GAIN_CWND;
    case BBR_PROBE_BW: return 2.0;/*bbr_pbw_gain[bbr->probe_bw_phase];*/
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
    /* cwnd = 4 * MSS to drain queue and measure true min_rtt */
    bbr->cwnd_ = 16 * bbr->mss;
    LOG_DEBUG("[cc-bbr] → PROBE_RTT  cwnd=%llu", (unsigned long long)bbr->cwnd_);
}

static void bbr_handle_round_end(quic_cc_bbr_t *bbr, uint64_t now_us) {
    /* compute bandwidth for completed round */
    uint64_t elapsed_us = now_us - bbr->round_start_us;
    if (elapsed_us < 5000) elapsed_us = 5000;  /* floor: 5ms/round avoids false ~GB/s */
    /* bytes/sec = round_bytes * 1e6 / elapsed_us */
    uint64_t bw = (uint64_t)((double)bbr->round_bytes * 1e6
                             / (double)elapsed_us);
    bbr_update_max_bw(bbr, bw);

    LOG_DEBUG("[cc-bbr] round end: %lluB/%lluus = %llu B/s"
             "  max_bw=%llu state=%d cwnd=%llu",
             (unsigned long long)bbr->round_bytes,
             (unsigned long long)elapsed_us,
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

        /* Check PROBE_RTT: if min_rtt hasn't been updated in 10s */
        if (now_us - bbr->rtt_probe_us > BBR_RTT_WINDOW_MS * 1000)
            bbr_enter_probe_rtt(bbr);
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
}

/* ── 回调 ────────────────────────────────── */

static void bbr_on_packet_sent(struct quic_cc *cc, uint64_t pn,
                               uint64_t bytes, uint64_t now_ms) {
    (void)cc; (void)pn; (void)bytes; (void)now_ms;
}

static void bbr_on_packet_acked(struct quic_cc *cc, uint64_t pn,
                                uint64_t bytes, uint64_t rtt_us,
                                uint64_t now_ms) {
    (void)pn; (void)now_ms;

    quic_cc_bbr_t *bbr = (quic_cc_bbr_t*)cc->priv;
    uint64_t now_us = bbr_now_us();

    /* update min_rtt — BBR only lowers, tracking the path floor */
    if (rtt_us > 0 && rtt_us < 10000000) {
        uint64_t r = rtt_us < BBR_MIN_RTT_US ? BBR_MIN_RTT_US : rtt_us;
        if (r < bbr->min_rtt_us || bbr->min_rtt_us == 0) {
            bbr->min_rtt_us = r;
            bbr->rtt_probe_us = now_us;
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
            LOG_DEBUG("[cc-bbr] STARTUP: cwnd=%llu (+%llu) rtt=%llu bw=%llu",
                     (unsigned long long)bbr->cwnd_, (unsigned long long)bytes,
                     (unsigned long long)rtt_us,
                     (unsigned long long)bbr->max_bw);
        }
    }

    /* end round when we've sent ~1 cwnd worth of data since round start.
     * In BBR, a round ends when we ACK a packet sent after the round start. */
    if (bbr->round_bytes >= bbr->cwnd_ / 2) {
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
    const quic_cc_bbr_t *bbr = (const quic_cc_bbr_t*)cc->priv;
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
    bbr->rtt_probe_us = bbr_now_us();
    bbr->max_bw       = bbr->mss * 100; /* 120KB/s minimal */

    cc->ops  = &bbr_ops;
    cc->priv = bbr;

    LOG_DEBUG("[cc-bbr] created: cwnd=%llu mss=%llu",
             (unsigned long long)bbr->cwnd_, (unsigned long long)bbr->mss);
    return cc;
}
