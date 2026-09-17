#include "quic_cc.h"
#include "quic_common.h"
#include "logger.h"
#include <stdlib.h>
#include <string.h>

/* ============================================
 * NewReno 拥塞控制 (RFC 9002)
 * ============================================ */

typedef struct {
    uint64_t cwnd_;
    uint64_t ssthresh_;           /* slow start threshold */
    uint64_t bytes_acked_ca_;     /* ACKed bytes accumulator for CA */
    int      in_recovery_;        /* 1 = waiting for recovery ACK */
} quic_cc_newreno_t;

/* ── 回调 ────────────────────────────────── */

static void nr_on_packet_sent(struct quic_cc *cc, uint64_t pn,
                              uint64_t bytes, uint64_t now_ms) {
    (void)cc; (void)pn; (void)bytes; (void)now_ms;
}

static void nr_on_packet_acked(struct quic_cc *cc, uint64_t pn,
                               uint64_t bytes, uint64_t rtt_us,
                               uint64_t now_ms) {
    (void)pn; (void)rtt_us; (void)now_ms;

    quic_cc_newreno_t *nr = (quic_cc_newreno_t*)cc->priv;

    if (nr->cwnd_ < nr->ssthresh_) {
        /* Slow Start: cwnd += acked_bytes (RFC 9002 §B.1.1) */
        nr->cwnd_ += bytes;
        LOG_DEBUG("[cc-newreno] pn=%llu acked %lluB: cwnd=%llu (slow start)",
                  (unsigned long long)pn, (unsigned long long)bytes,
                  (unsigned long long)nr->cwnd_);
    } else {
        /* Congestion Avoidance: AIMD per RFC 5681 §3.1
         * cwnd += SMSS * acked_bytes / cwnd
         * Accumulate fractional bytes; add SMSS when we've
         * ACKed a full cwnd worth of data (~1 MSS per RTT). */
        nr->bytes_acked_ca_ += bytes;
        uint64_t mss = QUIC_MIN_PKT_SIZE;
        while (nr->bytes_acked_ca_ >= nr->cwnd_) {
            nr->bytes_acked_ca_ -= nr->cwnd_;
            nr->cwnd_ += mss;
        }
    }
}

static void nr_on_congestion_event(struct quic_cc *cc, uint64_t now_ms) {
    (void)now_ms;

    quic_cc_newreno_t *nr = (quic_cc_newreno_t*)cc->priv;

    /* RFC 9002 §B.1.2: ssthresh = max(cwnd/2, 2*MSS) */
    nr->ssthresh_ = nr->cwnd_ / 2;
    if (nr->ssthresh_ < 2 * QUIC_MIN_PKT_SIZE)
        nr->ssthresh_ = 2 * QUIC_MIN_PKT_SIZE;

    /* RFC 9002 §B.1.2: cwnd = ssthresh (fast recovery) */
    nr->cwnd_ = nr->ssthresh_;
    nr->bytes_acked_ca_ = 0;

    LOG_DEBUG("[cc-newreno] congestion: cwnd=%llu ssthresh=%llu",
              (unsigned long long)nr->cwnd_,
              (unsigned long long)nr->ssthresh_);
}

static void nr_on_persistent_congestion(struct quic_cc *cc, uint64_t now_ms) {
    (void)now_ms;

    quic_cc_newreno_t *nr = (quic_cc_newreno_t*)cc->priv;
    /* RFC 9002 §6.2: cwnd = 2 * max_datagram_size */
    nr->cwnd_ = 2 * QUIC_MIN_PKT_SIZE;
    nr->ssthresh_ = 2 * QUIC_MIN_PKT_SIZE;
    nr->bytes_acked_ca_ = 0;

    LOG_DEBUG("[cc-newreno] persistent congestion: cwnd reset to %llu",
             (unsigned long long)nr->cwnd_);
}

static uint64_t nr_get_cwnd(const struct quic_cc *cc) {
    const quic_cc_newreno_t *nr = (const quic_cc_newreno_t*)cc->priv;
    return nr->cwnd_;
}

static void nr_get_info(const struct quic_cc *cc, struct quic_cc_info *out) {
    const quic_cc_newreno_t *nr = (const quic_cc_newreno_t*)cc->priv;
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->cwnd = nr->cwnd_;
    out->ssthresh = (nr->ssthresh_ == UINT64_MAX) ? 0 : nr->ssthresh_;
    out->algo = QUIC_CC_ALGO_NEWRENO;
    out->state = (nr->cwnd_ < nr->ssthresh_) ? 0 : 1; /* 0=slow start, 1=CA */
}

static void nr_destroy(struct quic_cc *cc) {
    free(cc->priv);
    free(cc);
}

/* ── 工厂 ────────────────────────────────── */

static const struct quic_cc_ops nr_ops = {
    .on_packet_sent      = nr_on_packet_sent,
    .on_packet_acked     = nr_on_packet_acked,
    .on_congestion_event       = nr_on_congestion_event,
    .on_persistent_congestion  = nr_on_persistent_congestion,
    .get_cwnd                  = nr_get_cwnd,
    .get_info                  = nr_get_info,
    .destroy             = nr_destroy,
};

struct quic_cc *quic_cc_newreno_create(void) {
    struct quic_cc *cc = (struct quic_cc*)calloc(1, sizeof(*cc));
    if (!cc) return NULL;

    quic_cc_newreno_t *nr = (quic_cc_newreno_t*)calloc(1, sizeof(*nr));
    if (!nr) { free(cc); return NULL; }

    /* 256*MSS (~300KB) — larger than payload, one-CWND round trips */
    nr->cwnd_       = 256 * QUIC_MIN_PKT_SIZE;
    nr->ssthresh_    = UINT64_MAX;
    nr->bytes_acked_ca_ = 0;
    nr->in_recovery_ = 0;

    cc->ops  = &nr_ops;
    cc->priv = nr;

    LOG_INFO("[cc-newreno] created: cwnd=%llu",
             (unsigned long long)nr->cwnd_);
    return cc;
}
