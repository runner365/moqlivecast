#include "quic_recovery.h"
#include "quic_cc.h"
#include "logger.h"
#include <stdlib.h>
#include <string.h>

extern int quic_frame_parse_stream(const uint8_t *data, size_t len,
                            uint64_t *stream_id, uint64_t *offset, int *fin,
                            const uint8_t **stream_data, size_t *stream_len);
extern int quic_varint_decode(const uint8_t *data, size_t len, uint64_t *val);

/* ── pkt_type → TLS level（PN 空间） ─────────────── */
static inline int pkt_type_to_level(int pkt_type) {
    if (pkt_type == QUIC_PKT_INITIAL)   return QUIC_TLS_LEVEL_NONE;
    if (pkt_type == QUIC_PKT_HANDSHAKE) return QUIC_TLS_LEVEL_HANDSHAKE;
    return QUIC_TLS_LEVEL_APPLICATION;   /* -1 短头 */
}

/* ── 环形缓冲辅助 ──────────────────────────────── */

static inline int slot_oldest_to_newest(QuicRecoveryCtx *ctx, int logical_pos) {
    return (ctx->sent_head_ - ctx->sent_count_ + logical_pos
            + QUIC_RECOVERY_MAX_SENT_PACKETS)
           % QUIC_RECOVERY_MAX_SENT_PACKETS;
}

/* ── 诊断：跳过 ACK/ACK_ECN 帧，返回其总长度（含 type 字节）；失败 -1 ── */
static int audit_ack_frame_len(const uint8_t *data, size_t len) {
    if (len < 1) return -1;
    if (data[0] != QUIC_FRAME_ACK && data[0] != QUIC_FRAME_ACK_ECN) return -1;
    size_t pos = 1;
    uint64_t v = 0;
    int r;
    /* largest_acknowledged / ack_delay / ack_range_count / first_ack_range */
    r = quic_varint_decode(data + pos, len - pos, &v); if (r < 0) return -1; pos += (size_t)r;
    r = quic_varint_decode(data + pos, len - pos, &v); if (r < 0) return -1; pos += (size_t)r;
    r = quic_varint_decode(data + pos, len - pos, &v); if (r < 0) return -1; pos += (size_t)r;
    uint64_t range_count = v;
    r = quic_varint_decode(data + pos, len - pos, &v); if (r < 0) return -1; pos += (size_t)r;
    /* range_count × (gap, ack_range) */
    for (uint64_t i = 0; i < range_count; i++) {
        r = quic_varint_decode(data + pos, len - pos, &v); if (r < 0) return -1; pos += (size_t)r;
        r = quic_varint_decode(data + pos, len - pos, &v); if (r < 0) return -1; pos += (size_t)r;
    }
    if (data[0] == QUIC_FRAME_ACK_ECN) {
        for (int e = 0; e < 3; e++) {
            r = quic_varint_decode(data + pos, len - pos, &v); if (r < 0) return -1; pos += (size_t)r;
        }
    }
    return (int)pos;
}

/* ── 诊断：打印所有「未确认」sent_packet 里的 STREAM 帧 (pn, stream_id, offset, len)。
 * 用于核对服务端发出的 offset 序列 与 客户端 ACK 的对应关系。
 * tag: 调用来源标记（如 " ack"/" loss"）。每条最多打 40 行，避免刷爆。 ── */
static void print_unack_packet(QuicRecoveryCtx *ctx, const char *tag) {
    if (!ctx) return;
    int unacked_total = 0, printed = 0;
    for (int i = 0; i < ctx->sent_count_; i++) {
        int idx = slot_oldest_to_newest(ctx, i);
        QuicRecoverySentPacket *p = &ctx->sent_packets_[idx];
        if (p->acknowledged) continue;
        unacked_total++;

        if (!p->frames || p->frames_len == 0) continue;
        const uint8_t *fp = p->frames;
        size_t flen = p->frames_len;
        while (flen > 0) {
            uint8_t ft = fp[0];
            if ((ft & 0xf8) == QUIC_FRAME_STREAM) {          /* 0x08–0x0f */
                uint64_t sid = 0, off = 0;
                int fin = 0;
                const uint8_t *sd = NULL;
                size_t sl = 0;
                int used = quic_frame_parse_stream(fp, flen, &sid, &off, &fin, &sd, &sl);
                if (used <= 0) break;
                if (printed < 40) {
                    LOG_DEBUG("[ack-audit]%s pn=%llu stream=%llu off=%llu len=%zu end=%llu lost=%d chunk=%llu",
                             tag ? tag : "",
                             (unsigned long long)p->pn,
                             (unsigned long long)sid,
                             (unsigned long long)off, sl,
                             (unsigned long long)(off + sl),
                             p->lost, (unsigned long long)p->chunk_id);
                    printed++;
                }
                fp += used; flen -= (size_t)used;
            } else if (ft == QUIC_FRAME_ACK || ft == QUIC_FRAME_ACK_ECN) {
                int used = audit_ack_frame_len(fp, flen);
                if (used <= 0) break;
                fp += used; flen -= (size_t)used;
            } else {
                break;   /* 其它帧（MAX_DATA 等）不解析，停止本包 */
            }
        }
    }
    LOG_DEBUG("[ack-audit]%s summary: unacked_pkts=%d printed=%d", tag ? tag : "", unacked_total, printed);
}

/* inflight 扣减 — 带 underflow 检测 + 饱和保护（防 unsigned 翻转）。
 * 正常情况下 bytes_in_flight_ 一定 ≥ 被扣的包字节数（+/- 成对）。
 * 若出现 underflow，说明某处多扣了一次（记账不对称），记日志并饱和到 0，
 * 避免 unsigned 翻转成巨大值导致 cwnd 判断永久阻塞。 */
static inline void inflight_sub(QuicRecoveryCtx *ctx, uint64_t bytes) {
    if (ctx->bytes_in_flight_ < bytes) {
        LOG_ERROR("[quic-recovery] inflight UNDERFLOW: %llu < %llu (accounting bug)",
                  (unsigned long long)ctx->bytes_in_flight_,
                  (unsigned long long)bytes);
        ctx->bytes_in_flight_ = 0;
    } else {
        ctx->bytes_in_flight_ -= bytes;
    }
}

/* ── RTT 更新 ────────────────────────────────────── */

static void update_rtt(QuicRecoveryCtx *ctx, uint64_t rtt_sample) {
    ctx->latest_rtt_ = rtt_sample;
    if (!ctx->rtt_initialized_) {
        ctx->min_rtt_ = ctx->smoothed_rtt_ = rtt_sample;
        ctx->rttvar_ = rtt_sample / 2;
        ctx->jitter_ms_ = 0;
        ctx->prev_rtt_sample_ = rtt_sample;
        ctx->rtt_initialized_ = 1;
    } else {
        int64_t diff = (int64_t)ctx->smoothed_rtt_ - (int64_t)rtt_sample;
        uint64_t abs_diff = (uint64_t)(diff < 0 ? -diff : diff);
        ctx->rttvar_ = (3 * ctx->rttvar_ + abs_diff) / 4;
        ctx->smoothed_rtt_ = (7 * ctx->smoothed_rtt_ + rtt_sample) / 8;
        if (rtt_sample < ctx->min_rtt_) ctx->min_rtt_ = rtt_sample;
        /* RFC 3550 §6.4.1 jitter: J += (|D| - J) / 16 */
        {
            int64_t d = (int64_t)rtt_sample - (int64_t)ctx->prev_rtt_sample_;
            uint64_t ad = (uint64_t)(d < 0 ? -d : d);
            int64_t j = (int64_t)ctx->jitter_ms_
                        + ((int64_t)ad - (int64_t)ctx->jitter_ms_) / 16;
            ctx->jitter_ms_ = j < 0 ? 0 : (uint64_t)j;
            ctx->prev_rtt_sample_ = rtt_sample;
        }
    }
    /* PTO = smoothed_rtt + max(4*rttvar, min) (RFC 9002 §6.2.1) */
    uint64_t base = ctx->smoothed_rtt_ + 4 * ctx->rttvar_;
    if (base < QUIC_MIN_PTO_MS) base = QUIC_MIN_PTO_MS;
    ctx->pto_base_ = base;

    if (ctx->pto_base_ > 100 || ctx->smoothed_rtt_ > 100 || rtt_sample > 100) {
        LOG_WARN("[quic-recovery] RTT large rtt: rtt_sample=%llums → srtt=%llu rttvar=%llu min_rtt=%llu pto_base=%llu",
             (unsigned long long)rtt_sample,
             (unsigned long long)ctx->smoothed_rtt_,
             (unsigned long long)ctx->rttvar_,
             (unsigned long long)ctx->min_rtt_,
             (unsigned long long)ctx->pto_base_);
    }

}

/* ── Chunk 管理 ──────────────────────────────────── */

static QuicRecoveryChunk *chunk_find(QuicRecoveryCtx *ctx, uint64_t chunk_id) {
    for (int i = 0; i < ctx->chunk_count_; i++)
        if (ctx->chunks_[i].chunk_id == chunk_id) return &ctx->chunks_[i];
    return NULL;
}

static QuicRecoveryChunk *chunk_alloc(QuicRecoveryCtx *ctx) {
    if (ctx->chunk_count_ < QUIC_RECOVERY_MAX_CHUNKS) {
        QuicRecoveryChunk *c = &ctx->chunks_[ctx->chunk_count_++];
        memset(c, 0, sizeof(*c));
        c->chunk_id = ctx->next_chunk_id_++;
        return c;
    }
    /* 满了 → 清理已 ACK 的 chunk */
    int w = 0;
    for (int i = 0; i < ctx->chunk_count_; i++) {
        if (ctx->chunks_[i].state == QUIC_CHUNK_ACKED) {
            free(ctx->chunks_[i].frames);
        } else {
            ctx->chunks_[w++] = ctx->chunks_[i];
        }
    }
    ctx->chunk_count_ = w;
    if (w < QUIC_RECOVERY_MAX_CHUNKS) {
        QuicRecoveryChunk *c = &ctx->chunks_[ctx->chunk_count_++];
        memset(c, 0, sizeof(*c));
        c->chunk_id = ctx->next_chunk_id_++;
        return c;
    }
    return NULL;
}

/* ACK 到达 → 标记 chunk 及其所有 sent_packet 为已确认 */
static void chunk_mark_acked(QuicRecoveryCtx *ctx, uint64_t chunk_id) {
    QuicRecoveryChunk *c = chunk_find(ctx, chunk_id);
    if (!c || c->state == QUIC_CHUNK_ACKED) return;
    c->state = QUIC_CHUNK_ACKED;
    free(c->frames); c->frames = NULL; c->frames_len = 0;

    for (int i = 0; i < ctx->sent_count_; i++) {
        int idx = slot_oldest_to_newest(ctx, i);
        QuicRecoverySentPacket *p = &ctx->sent_packets_[idx];
        if (p->chunk_id == chunk_id && !p->acknowledged) {
            /* 同 chunk 的重传副本被 ACK 覆盖 → 同样要扣 inflight。
             * 只扣未 lost 的（lost 的已在 detect_losses 扣过），
             * 只扣 ack_eliciting 的（非 ack-eliciting 从未 inflight+=）。 */
            if (p->ack_eliciting && !p->lost)
                inflight_sub(ctx, p->bytes_sent);
            p->acknowledged = 1;
            free(p->frames); p->frames = NULL; p->frames_len = 0;
        }
    }
}

/* ── 标记单个 PN 已确认 ────────────────────────── */

static void mark_pn_acked(QuicRecoveryCtx *ctx, uint64_t pn, int level,
                           uint64_t now_ms, int *newly_acked_count,
                           uint64_t *largest_newly_acked_pn) {
    for (int i = 0; i < ctx->sent_count_; i++) {
        int idx = slot_oldest_to_newest(ctx, i);
        QuicRecoverySentPacket *p = &ctx->sent_packets_[idx];
        if (p->pn != pn || p->acknowledged) continue;

        /* PN 空间隔离 — Initial/Handshake/Application 的 PN 各自独立，
         * 只确认属于同一 level 的包，避免跨空间误匹配 */
        if (pkt_type_to_level(p->pkt_type) != level) continue;
        if (p->pn == pn && p->ack_eliciting && p->lost) {
            LOG_DEBUG("[quic-recovery] pn=%llu acked BUT lost (retransmitted?) chunk=%llu",
                     (unsigned long long)pn,
                     (unsigned long long)p->chunk_id);
        }
        p->acknowledged = 1;
        (*newly_acked_count)++;
        ctx->packets_acked_++;

        /* RTT 采样（RFC 9002 §5.2）：只用「未丢失/未重传过」的 largest
         * newly-acked 包。lost 的包被 ACK 覆盖时，now-time_sent 是
         * 「首次发送→丢包→重传→ACK」的总时间（可达秒级），会把
         * srtt/rttvar 污染到巨大值（如 srtt=760 而真实 RTT ~100ms）。 */
        if (p->ack_eliciting && !p->lost && pn > *largest_newly_acked_pn) {
            *largest_newly_acked_pn = pn;
            uint64_t rtt = now_ms - p->time_sent_ms;
            update_rtt(ctx, rtt);
        }
        if (p->ack_eliciting && !p->lost) {
            inflight_sub(ctx, p->bytes_sent);
            /* 握手包（Initial/Handshake）只记账，不驱动 CC/cwnd */
            if (ctx->cc_ && p->pkt_type == -1) {
                uint64_t rtt = now_ms - p->time_sent_ms;
                ctx->cc_->ops->on_packet_acked(ctx->cc_, pn,
                                               p->bytes_sent, rtt * 1000, now_ms);
            }
        }

        /* 整个 chunk 标记为已确认 — 清理同 chunk 其他 sent_packet */
        chunk_mark_acked(ctx, p->chunk_id);

        LOG_DEBUG("[quic-recovery] pn=%llu acked (rtt=%llums)",
                  (unsigned long long)pn,
                  (unsigned long long)(now_ms - p->time_sent_ms));
        return;
    }
}

/* ── Compact ────────────────────────────────────── */

static void compact_sent_packets(QuicRecoveryCtx *ctx) {
    QuicRecoverySentPacket tmp[QUIC_RECOVERY_MAX_SENT_PACKETS];
    int new_count = 0;
    for (int i = 0; i < ctx->sent_count_; i++) {
        int idx = slot_oldest_to_newest(ctx, i);
        QuicRecoverySentPacket *p = &ctx->sent_packets_[idx];
        if (p->acknowledged) {
            free(p->frames);
            continue;
        }
        if (p->lost && !p->frames) continue;
        tmp[new_count++] = *p;
    }
    if (new_count < ctx->sent_count_) {
        memcpy(ctx->sent_packets_, tmp, (size_t)new_count * sizeof(QuicRecoverySentPacket));
        ctx->sent_count_ = new_count;
        ctx->sent_head_  = new_count;
    }
}

/* ── ACK 范围 ───────────────────────────────────── */

static int process_ack_ranges(QuicRecoveryCtx *ctx, const QuicAckFrame *ack,
                               uint64_t now_ms, int level) {
    int newly_acked_count = 0;
    uint64_t largest_newly_acked_pn = 0;

    if (ack->largest_acknowledged > ctx->largest_acked_pn_[level])
        ctx->largest_acked_pn_[level] = ack->largest_acknowledged;

    uint64_t range_start = ack->largest_acknowledged - ack->first_ack_range;
    uint64_t range_end   = ack->largest_acknowledged;
    for (uint64_t pn = range_start; pn <= range_end; pn++)
        mark_pn_acked(ctx, pn, level, now_ms, &newly_acked_count, &largest_newly_acked_pn);

    uint64_t prev_range_min = range_start;
    for (int i = 0; i < ack->num_ranges; i++) {
        uint64_t r_end = prev_range_min - ack->gap[i] - 2;
        uint64_t r_start = r_end - ack->ack_range[i];
        for (uint64_t pn = r_start; pn <= r_end; pn++)
            mark_pn_acked(ctx, pn, level, now_ms, &newly_acked_count, &largest_newly_acked_pn);
        prev_range_min = r_start;
    }
    if (newly_acked_count == 0) {
        LOG_DEBUG("[quic-recovery] ACK no progress largest=%llu sent=%d",
             (unsigned long long)ack->largest_acknowledged,
             ctx->sent_count_);
    }
    return newly_acked_count > 0;
}

/* ── 丢包检测 ──────────────────────────────────── */

static void detect_losses(QuicRecoveryCtx *ctx, uint64_t now_ms) {
    int loss_detected = 0;
    int app_loss_detected = 0;   /* 是否有应用层（1-RTT）丢包 */
    uint64_t rtt_ref = ctx->smoothed_rtt_ > ctx->latest_rtt_ ?
                       ctx->smoothed_rtt_ : ctx->latest_rtt_;
    if (rtt_ref < ctx->pto_base_) rtt_ref = ctx->pto_base_;
    uint64_t time_threshold = (9 * rtt_ref) / 8;
    if (time_threshold < 1) time_threshold = 1;

    LOG_DEBUG("[quic-recovery] DETECT_LOSSES srtt=%llu latest=%llu rttvar=%llu"
             " pto_base=%llu time_thr=%llu sent=%d",
             (unsigned long long)ctx->smoothed_rtt_,
             (unsigned long long)ctx->latest_rtt_,
             (unsigned long long)ctx->rttvar_,
             (unsigned long long)ctx->pto_base_,
             (unsigned long long)time_threshold,
             ctx->sent_count_);

    for (int i = 0; i < ctx->sent_count_; i++) {
        int idx = slot_oldest_to_newest(ctx, i);
        QuicRecoverySentPacket *p = &ctx->sent_packets_[idx];
        if (p->acknowledged || p->lost) continue;

        int lv = pkt_type_to_level(p->pkt_type);
        if (p->pn <= ctx->largest_acked_pn_[lv]) {
            LOG_DEBUG("[quic-recovery] LOSS pn=%llu BUT largest_acked=%llu (already acked?!) chunk=%llu",
                     (unsigned long long)p->pn,
                     (unsigned long long)ctx->largest_acked_pn_[lv],
                     (unsigned long long)p->chunk_id);
        }
        int64_t pn_diff = (int64_t)ctx->largest_acked_pn_[lv] - (int64_t)p->pn;
        int pn_threshold_met = (pn_diff >= 3);
        int time_threshold_met = (now_ms - p->time_sent_ms > time_threshold);

        if (!(pn_threshold_met || time_threshold_met)) continue;
        if (!p->ack_eliciting || !p->frames) continue;

        /* 检查 chunk — 如果已经 LOST（已重传过，PTO 兜底）。
         * 这些陈旧的重传 sent_packet 必须标记 lost=1，否则它们稍后
         * 被对端 ACK 覆盖时（!p->lost 为真），会用「首次发送时刻」
         * 算出秒级 RTT（1538/6921ms），污染 srtt/rttvar → 恶性循环
         * 让 pto_base/time_threshold 膨胀到秒级。 */
        QuicRecoveryChunk *c = chunk_find(ctx, p->chunk_id);
        if (c && c->state == QUIC_CHUNK_LOST) {
            if (p->ack_eliciting && !p->lost)
                inflight_sub(ctx, p->bytes_sent);
            p->lost = 1;
            free(p->frames); p->frames = NULL; p->frames_len = 0;
            continue;
        }

        p->lost = 1;
        ctx->packets_lost_++;
        loss_detected = 1;
        if ((now_ms/1000) != ctx->last_loss_log_s_) {
            ctx->last_loss_log_s_ = now_ms/1000;
            LOG_DEBUG("[quic-recovery] LOSS pn=%llu chunk=%llu bif=%llu retrans=%zuB",
                    (unsigned long long)p->pn, (unsigned long long)p->chunk_id,
                    (unsigned long long)ctx->bytes_in_flight_,
                    p->frames_len);
        }
        if (p->pkt_type == -1) app_loss_detected = 1;

        /* 首次丢包 → 立即重传（直接 UDP，不等任何定时器） */
        if (ctx->send_imm_fn) {
            ctx->current_retrans_chunk_id_ = p->chunk_id;
            ctx->send_imm_fn(ctx->conn_, p->pkt_type, p->frames, p->frames_len);
        }
        LOG_DEBUG("[quic-recovery] chunk=%llu pn=%llu LOST → retransmit"
                 " (pn_diff=%lld time=%llums)",
                 (unsigned long long)p->chunk_id, (unsigned long long)p->pn,
                 (long long)pn_diff,
                 (unsigned long long)(now_ms - p->time_sent_ms));

        inflight_sub(ctx, p->bytes_sent);
        free(p->frames); p->frames = NULL; p->frames_len = 0;

        /* Chunk 进入 LOST 状态 — 后续同 chunk 的重传不会重复触发 */
        if (c) c->state = QUIC_CHUNK_LOST;
    }

    if (loss_detected) {
        if (!ctx->loss_epoch_start_ms_)
            ctx->loss_epoch_start_ms_ = now_ms;

        /* CC 拥塞事件只由应用层（1-RTT）丢包触发；
         * 握手期 Initial/Handshake 丢包频繁，不污染 cwnd */
        uint64_t persistent_threshold = ctx->pto_base_ * QUIC_PERSISTENT_CONGESTION_THRESHOLD;
        if (app_loss_detected &&
            !ctx->persistent_congestion_detected_ &&
            now_ms - ctx->loss_epoch_start_ms_ >= persistent_threshold &&
            ctx->pto_count_ >= QUIC_PERSISTENT_CONGESTION_THRESHOLD) {
            ctx->persistent_congestion_detected_ = 1;
            if (ctx->cc_)
                ctx->cc_->ops->on_persistent_congestion(ctx->cc_, now_ms);
        }
        if (ctx->cc_ && app_loss_detected)
            ctx->cc_->ops->on_congestion_event(ctx->cc_, now_ms);
        ctx->flush_fn(ctx->conn_);
    } else {
        ctx->loss_epoch_start_ms_ = 0;
        ctx->persistent_congestion_detected_ = 0;
    }
}

/* ── PTO 定时器 ────────────────────────────────── */

static void arm_pto_timer(QuicRecoveryCtx *ctx);

static void on_pto_timer(void *user) {
    QuicRecoveryCtx *ctx = (QuicRecoveryCtx*)user;
    if (!ctx || !ctx->conn_) return;

    uint64_t now = uv_now(ctx->loop_);

    detect_losses(ctx, now);
    compact_sent_packets(ctx);

    if ((now / 1000) != ctx->last_ack_log_s_) {
        ctx->last_ack_log_s_ = now / 1000;
        print_unack_packet(ctx, "ACK");
    }

    /* 自愈：sent_packets 空但 inflight 残留 → 清零 + 发送 keepalive 触 ACK */
    if (ctx->sent_count_ == 0 && ctx->bytes_in_flight_ > 0) {
        LOG_DEBUG("[quic-recovery] inflight leak %llu→0 + keepalive",
                 (unsigned long long)ctx->bytes_in_flight_);
        ctx->bytes_in_flight_ = 0;
        if (ctx->on_pto_keepalive) ctx->on_pto_keepalive(ctx->conn_);
        ctx->last_keepalive_ms_ = now;
    }

    /* 每 20 次 PTO dump 一次 chunk 分类统计 + 前 8 字节 hex */
    static int pto_dump_counter = 0;
    if (++pto_dump_counter % 20 == 1) {
        int unacked = 0, chunks_alive = 0;
        for (int j = 0; j < ctx->sent_count_; j++) {
            int sidx = slot_oldest_to_newest(ctx, j);
            if (!ctx->sent_packets_[sidx].acknowledged) unacked++;
        }
        for (int j = 0; j < ctx->chunk_count_; j++)
            if (ctx->chunks_[j].state != QUIC_CHUNK_ACKED) chunks_alive++;
        LOG_DEBUG("[quic-recovery] PTO#%llu: sent=%d unacked=%d"
                 " chunks=%d(alive=%d) inflight=%llu",
                 (unsigned long long)ctx->pto_count_,
                 ctx->sent_count_, unacked,
                 ctx->chunk_count_, chunks_alive,
                 (unsigned long long)ctx->bytes_in_flight_);
        /* dump 每个 alive chunk 的前 8 字节 hex */
        int dumped = 0;
        for (int j = 0; j < ctx->chunk_count_ && dumped < 10; j++) {
            QuicRecoveryChunk *c = &ctx->chunks_[j];
            if (c->state == QUIC_CHUNK_ACKED) continue;
            char hex[33]; hex[0] = 0;
            if (c->frames_len > 0) {
                size_t n = c->frames_len < 16 ? c->frames_len : 16;
                for (size_t k = 0; k < n; k++)
                    snprintf(hex + k*2, 3, "%02x", c->frames[k]);
            }
            LOG_DEBUG("[quic-recovery]   chunk=%llu sz=%zuB st=%d rtx=%d hex[16]=%s",
                     (unsigned long long)c->chunk_id, c->frames_len,
                     c->state, c->retrans_count, hex);
            dumped++;
        }
    }

    /* PTO 兜底：遍历 chunk，找到未 ACK 的 → 直接 UDP 重传 */
    for (int i = 0; i < ctx->chunk_count_; i++) {
        QuicRecoveryChunk *c = &ctx->chunks_[i];
        if (c->state == QUIC_CHUNK_ACKED || !c->frames) continue;

        /* PTO 冷却：20 次重传后放弃 + 同 chunk 50ms 间隔防叠加。
         * 放弃上限仅对应用层（1-RTT）生效；握手空间（Initial/Handshake）
         * 豁免，持续重传到握手完成清理，避免弱网握手死锁。 */
        if (c->retrans_count >= 20 && c->pkt_type == -1) {
            LOG_WARN("[quic-recovery] PTO retrans over count:%d chunk=%llu %zuB state=%d"
                     " retrans=%d",
                     c->retrans_count,
                     (unsigned long long)c->chunk_id, c->frames_len,
                     c->state, c->retrans_count);
            if(ctx->on_connection_dead) {
                LOG_WARN("[quic-recovery] PTO retrans over call on_connection_dead");
                ctx->on_connection_dead(ctx->conn_);
                return;
            }
        }
        if (now - c->last_retrans_ms < 10) continue;

        LOG_DEBUG("[quic-recovery] PTO retrans: chunk=%llu %zuB state=%d"
                 " retrans=%d",
                 (unsigned long long)c->chunk_id, c->frames_len,
                 c->state, c->retrans_count);
        if (ctx->send_imm_fn) {
            ctx->current_retrans_chunk_id_ = c->chunk_id;
            LOG_DEBUG("[quic-recovery] PTO retrans chunk=%llu %zuB retrans=%d pto=%llu",
                     (unsigned long long)c->chunk_id, c->frames_len,
                     c->retrans_count, (unsigned long long)ctx->pto_count_);
            ctx->send_imm_fn(ctx->conn_, c->pkt_type, c->frames, c->frames_len);
        }
        c->retrans_count++;
        c->last_retrans_ms = now;
    }

    if (now - ctx->last_keepalive_ms_ > 5000) {
        if (ctx->on_pto_keepalive) {
            ctx->on_pto_keepalive(ctx->conn_);
            ctx->flush_fn(ctx->conn_);
        }
        ctx->last_keepalive_ms_ = now;
    }
    ctx->pto_count_++;
    arm_pto_timer(ctx);
}

static void arm_pto_timer(QuicRecoveryCtx *ctx) {
    int has_unacked = 0;
    for (int i = 0; i < ctx->sent_count_; i++) {
        int idx = slot_oldest_to_newest(ctx, i);
        if (!ctx->sent_packets_[idx].acknowledged &&
            ctx->sent_packets_[idx].ack_eliciting) {
            has_unacked = 1; break;
        }
    }
    /* 没有未确认的 ack-eliciting 包 → 不需要 PTO，停掉定时器 */
    if (!has_unacked) {
        quic_timer_stop(&ctx->pto_timer_);
        return;
    }
    if (ctx->pto_count_ > 3) ctx->pto_count_ = 3;

    uint64_t timeout = ctx->pto_base_;
    for (uint64_t n = 0; n < ctx->pto_count_; n++) timeout *= 2;
    /* 不再 clamp 到 20ms — 让指数退避生效（200/400/800/1600ms），
     * 上限由 idle timeout 兜底。 */

    quic_timer_stop(&ctx->pto_timer_);
    if (ctx->fix_pto_timeout_ > 0) timeout = ctx->fix_pto_timeout_;
    quic_timer_start(&ctx->pto_timer_, on_pto_timer, ctx, timeout, 0);
}

/* ── 公开 API ──────────────────────────────────── */

void quic_recovery_init(QuicRecoveryCtx *ctx, void *conn, uv_loop_t *loop,
                         int (*queue_fn)(void *conn, const uint8_t *frames, size_t flen),
                         void (*flush_fn)(void *conn),
                         int (*send_imm_fn)(void *conn, int pkt_type,
                                            const uint8_t *data, size_t len),
                         struct quic_cc *cc) {
    memset(ctx, 0, sizeof(*ctx));
    ctx->conn_  = conn;
    ctx->loop_  = loop;
    ctx->queue_fn = queue_fn;
    ctx->flush_fn = flush_fn;
    ctx->send_imm_fn = send_imm_fn;
    /* CC 选择：改这一行即可在 BBR / CUBIC / NewReno 间切换（对照实验用） */
    ctx->cc_ = cc ? cc : quic_cc_bbr_create();
    // ctx->cc_ = cc ? cc : quic_cc_cubic_create();
    // ctx->cc_ = cc ? cc : quic_cc_newreno_create();
    ctx->pto_base_ = QUIC_INITIAL_PTO_MS;
    ctx->current_retrans_chunk_id_ = UINT64_MAX;  /* 未复用哨兵 */
    ctx->fix_pto_timeout_ = 30; // for debug pto timeout fix
    ctx->last_ack_log_s_ = 0;
    quic_timer_init(&ctx->pto_timer_);
}

void quic_recovery_cleanup(QuicRecoveryCtx *ctx) {
    quic_timer_stop(&ctx->pto_timer_);
    for (int i = 0; i < ctx->sent_count_; i++) {
        int idx = slot_oldest_to_newest(ctx, i);
        free(ctx->sent_packets_[idx].frames);
        ctx->sent_packets_[idx].frames = NULL;
    }
    ctx->sent_count_ = 0;
    for (int i = 0; i < ctx->chunk_count_; i++) {
        free(ctx->chunks_[i].frames);
        ctx->chunks_[i].frames = NULL;
    }
    ctx->chunk_count_ = 0;
    if (ctx->cc_) { ctx->cc_->ops->destroy(ctx->cc_); ctx->cc_ = NULL; }
}

void quic_recovery_on_packet_sent(QuicRecoveryCtx *ctx, uint64_t pn,
                                   int ack_eliciting, uint64_t bytes_sent,
                                   const uint8_t *frames, size_t frames_len,
                                   int pkt_type, uint64_t now_ms) {
    int idx, reuse_chunk = (ctx->current_retrans_chunk_id_ != UINT64_MAX);

    /* 缓冲区满 → compact */
    if (ctx->sent_count_ >= QUIC_RECOVERY_MAX_SENT_PACKETS)
        compact_sent_packets(ctx);

    if (ctx->sent_count_ >= QUIC_RECOVERY_MAX_SENT_PACKETS) {
        int oldest = slot_oldest_to_newest(ctx, 0);
        QuicRecoverySentPacket *old = &ctx->sent_packets_[oldest];
        if (old->ack_eliciting && !old->acknowledged && !old->lost)
            inflight_sub(ctx, old->bytes_sent);
        free(old->frames);
        idx = oldest;
    } else {
        idx = ctx->sent_head_; ctx->sent_count_++;
    }

    QuicRecoverySentPacket *p = &ctx->sent_packets_[idx];
    memset(p, 0, sizeof(*p));
    p->pn = pn;
    p->time_sent_ms = now_ms;
    p->bytes_sent   = bytes_sent;
    p->pkt_type     = pkt_type;
    p->ack_eliciting = ack_eliciting;

    /* ── Chunk 分配 ── */
    QuicRecoveryChunk *c = NULL;
    if (reuse_chunk) {
        c = chunk_find(ctx, ctx->current_retrans_chunk_id_);
        ctx->current_retrans_chunk_id_ = UINT64_MAX;
    }
    if (!c) {
        c = chunk_alloc(ctx);
        if (!c) {
            LOG_ERROR("[quic-recovery] chunk_alloc failed (max=%d), count=%d",
                      QUIC_RECOVERY_MAX_CHUNKS, ctx->chunk_count_);
        }
        if (c && frames_len > 0 && frames) {
            p->frames = (uint8_t*)malloc(frames_len);
            if (p->frames) {
                memcpy(p->frames, frames, frames_len);
                p->frames_len = frames_len;
            }

            c->state = QUIC_CHUNK_SENDING;
            c->first_sent_ms = now_ms;
            c->bytes = bytes_sent;
            c->pkt_type = pkt_type;
            c->frames = (uint8_t*)malloc(frames_len);
            if (c->frames) {
                memcpy(c->frames, frames, frames_len);
                c->frames_len = frames_len;
            }
        }
    } else {
        /* 重传复用现有 chunk → 只创建 sent_packet，不创建新数据 */
        if (frames_len > 0 && frames) {
            p->frames = (uint8_t*)malloc(frames_len);
            if (p->frames) {
                memcpy(p->frames, frames, frames_len);
                p->frames_len = frames_len;
            }
        }
        if (c->state == QUIC_CHUNK_SENDING)
            c->state = QUIC_CHUNK_LOST;
    }
    p->chunk_id = c ? c->chunk_id : UINT64_MAX;

    if (reuse_chunk && c && frames && frames_len > 0) {
        const uint8_t *fp = frames;
        size_t flen = frames_len;
        while (flen > 0 && (fp[0] & 0xf8) == QUIC_FRAME_STREAM) {
            uint64_t sid = 0, off = 0;
            int fin = 0;
            const uint8_t *sd = NULL;
            size_t sl = 0;
            int used = quic_frame_parse_stream(fp, flen, &sid, &off, &fin, &sd, &sl);
            if (used <= 0) {
                LOG_ERROR("[quic-recovery] parse_stream failed, used=%d", used);
                break;
            }
            LOG_DEBUG("[retrans-audit] new_pn=%llu chunk=%llu stream=%llu off=%llu len=%zu",
                     (unsigned long long)pn, (unsigned long long)c->chunk_id,
                     (unsigned long long)sid, (unsigned long long)off, sl);
            fp += used; flen -= (size_t)used;
        }
    }
    if (ack_eliciting)
        ctx->bytes_in_flight_ += bytes_sent;

    ctx->sent_head_ = (idx + 1) % QUIC_RECOVERY_MAX_SENT_PACKETS;

    LOG_DEBUG("[quic-recovery] SENT pn=%llu chunk=%llu%s ack=%d pkt_type=%d bytes=%llu inflight=%llu",
             (unsigned long long)pn, (unsigned long long)p->chunk_id,
             reuse_chunk ? " (retrans)" : "",
             ack_eliciting, pkt_type, (unsigned long long)bytes_sent,
             (unsigned long long)ctx->bytes_in_flight_);

    if (ack_eliciting && !quic_timer_is_active(&ctx->pto_timer_))
        arm_pto_timer(ctx);
}

void quic_recovery_on_ack_received(QuicRecoveryCtx *ctx,
                                    const QuicAckFrame *ack,
                                    uint64_t now_ms, int level) {
    int progress = process_ack_ranges(ctx, ack, now_ms, level);
    detect_losses(ctx, now_ms);
    compact_sent_packets(ctx);

    if (progress) ctx->pto_count_ = 0;
    arm_pto_timer(ctx);
}

void quic_recovery_check_losses(QuicRecoveryCtx *ctx) {
    if (!ctx || !ctx->loop_) return;
    uint64_t now = uv_now(ctx->loop_);
    detect_losses(ctx, now);
    compact_sent_packets(ctx);
}
/* ── 握手完成后清理 PN 空间 ─────────────────────── */

static void recovery_clear_space(QuicRecoveryCtx *ctx, int pkt_type) {
    for (int i = 0; i < ctx->sent_count_; i++) {
        int idx = slot_oldest_to_newest(ctx, i);
        QuicRecoverySentPacket *p = &ctx->sent_packets_[idx];
        if (p->acknowledged || p->pkt_type != pkt_type) continue;
        if (p->ack_eliciting && !p->lost)
            inflight_sub(ctx, p->bytes_sent);
        p->acknowledged = 1;
        free(p->frames); p->frames = NULL; p->frames_len = 0;
    }
    for (int i = 0; i < ctx->chunk_count_; i++) {
        QuicRecoveryChunk *c = &ctx->chunks_[i];
        if (c->state == QUIC_CHUNK_ACKED || c->pkt_type != pkt_type) continue;
        c->state = QUIC_CHUNK_ACKED;
        free(c->frames); c->frames = NULL; c->frames_len = 0;
    }
}

void quic_recovery_handshake_done(QuicRecoveryCtx *ctx, int is_server) {
    (void)is_server;
    /* 握手完成后两侧都只清 Initial 空间；Handshake 空间保留。
     * 客户端 Finished 是 Handshake 空间的最后一个包，可能丢，需重传
     * 直到对端 ACK（服务端收到 Finished 后 SSL_accept 才完成）。
     * Handshake 空间在收到对端首个 1-RTT 包后由 quic_recovery_clear_handshake 清理。 */
    recovery_clear_space(ctx, QUIC_PKT_INITIAL);
    arm_pto_timer(ctx);
}

void quic_recovery_clear_handshake(QuicRecoveryCtx *ctx) {
    recovery_clear_space(ctx, QUIC_PKT_HANDSHAKE);
    arm_pto_timer(ctx);
}

uint64_t quic_recovery_get_srtt(const QuicRecoveryCtx *ctx)
    { return ctx->smoothed_rtt_; }
uint64_t quic_recovery_get_bytes_in_flight(const QuicRecoveryCtx *ctx)
    { return ctx->bytes_in_flight_; }

int quic_recovery_can_send(const QuicRecoveryCtx *ctx) {
    if (!ctx->cc_) return 0;
    return quic_cc_can_send(ctx->cc_, ctx->bytes_in_flight_);
}

void quic_recovery_reset_pto(QuicRecoveryCtx *ctx) {
    quic_timer_stop(&ctx->pto_timer_);
    ctx->pto_count_ = 0;
}
