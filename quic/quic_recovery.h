#ifndef QUIC_RECOVERY_H
#define QUIC_RECOVERY_H

#include "quic_common.h"
#include "quic_cc.h"
#include "quic_timer.h"
#include <uv.h>

#ifdef __cplusplus
extern "C" {
#endif

#define QUIC_RECOVERY_MAX_SENT_PACKETS 4096
#define QUIC_RECOVERY_MAX_CHUNKS       512
#define QUIC_RECOVERY_MAX_PTO          20

/* PTO 定时（RFC 9002 §6.2） */
#define QUIC_INITIAL_PTO_MS   200   /* 握手前固定初始 PTO（无 RTT 样本时） */
#define QUIC_MIN_PTO_MS        10   /* PTO 下限，防 0 */

#define QUIC_CHUNK_SENDING  0
#define QUIC_CHUNK_LOST     1
#define QUIC_CHUNK_ACKED    2

/* ── 数据块：同一份数据可以发送多次（多个 PN），共享一个 chunk ── */
typedef struct {
    uint64_t chunk_id;
    int      state;           /* CHUNK_SENDING / LOST / ACKED */
    uint64_t first_sent_ms;   /* 首次发送时间（丢包时间阈值用） */
    uint64_t bytes;
    uint8_t *frames;          /* 保留一份用于 PTO 兜底重传 */
    size_t   frames_len;
    int      retrans_count;
    int      pkt_type;        /* QUIC 包类型 (用于重传时选密钥/长头) */
    uint64_t last_retrans_ms; /* PTO 冷却 — 防止同 chunk 重复重传 */
} QuicRecoveryChunk;

/* ── 已发送包记录（独立 PN，但共享 chunk） ── */
typedef struct {
    uint64_t pn;
    uint64_t chunk_id;
    uint64_t time_sent_ms;
    uint64_t bytes_sent;
    int      ack_eliciting;
    int      acknowledged;
    int      lost;
    int      pkt_type;        /* QUIC 包类型 */
    uint8_t *frames;
    size_t   frames_len;
} QuicRecoverySentPacket;

typedef struct {
    QuicRecoverySentPacket sent_packets_[QUIC_RECOVERY_MAX_SENT_PACKETS];
    int  sent_head_;
    int  sent_count_;

    /* Chunk 数组 — 数据块追踪 */
    QuicRecoveryChunk chunks_[QUIC_RECOVERY_MAX_CHUNKS];
    int  chunk_count_;
    uint64_t next_chunk_id_;           /* 自增 ID */
    uint64_t current_retrans_chunk_id_; /* 临时：重传时复用 chunk_id（UINT64_MAX=未复用哨兵） */

    /* RTT (ms) */
    uint64_t min_rtt_;
    uint64_t smoothed_rtt_;
    uint64_t latest_rtt_;
    uint64_t rttvar_;
    uint64_t jitter_ms_;          /* RFC 3550 inter-arrival jitter */
    uint64_t prev_rtt_sample_;
    uint64_t packets_lost_;
    uint64_t packets_acked_;
    int      rtt_initialized_;

    /* PTO */
    uint64_t pto_count_;
    uint64_t pto_base_;

    #define QUIC_PERSISTENT_CONGESTION_THRESHOLD 3
    uint64_t loss_epoch_start_ms_;
    int      persistent_congestion_detected_;

    struct quic_cc *cc_;
    uint64_t bytes_in_flight_;
    uint64_t largest_acked_pn_[QUIC_TLS_LEVEL_NUM];  /* per-PN-space ACK 上界 */

    QuicTimer  pto_timer_;
    uint64_t   last_keepalive_ms_;
    uint64_t   last_loss_log_s_;
    uint64_t   last_ack_log_s_;
    void *conn_;
    int  (*queue_fn)(void *conn, const uint8_t *frames, size_t flen);
    void (*flush_fn)(void *conn);
    int  (*send_imm_fn)(void *conn, int pkt_type, const uint8_t *data, size_t len);
    void (*on_connection_dead)(void *conn);
    void (*on_pto_keepalive)(void *conn);

    uv_loop_t *loop_;
} QuicRecoveryCtx;

void quic_recovery_init(QuicRecoveryCtx *ctx, void *conn, uv_loop_t *loop,
                         int (*queue_fn)(void *conn, const uint8_t *frames, size_t flen),
                         void (*flush_fn)(void *conn),
                         int (*send_imm_fn)(void *conn, int pkt_type,
                                            const uint8_t *data, size_t len),
                         struct quic_cc *cc);
void quic_recovery_cleanup(QuicRecoveryCtx *ctx);

void quic_recovery_on_packet_sent(QuicRecoveryCtx *ctx, uint64_t pn,
                                   int ack_eliciting, uint64_t bytes_sent,
                                   const uint8_t *frames, size_t frames_len,
                                   int pkt_type, uint64_t now_ms);
void quic_recovery_on_ack_received(QuicRecoveryCtx *ctx,
                                    const QuicAckFrame *ack,
                                    uint64_t now_ms, int level);
/* flush 被 cwnd 挡住时调用：推进丢包检测以回收 bif */
void quic_recovery_check_losses(QuicRecoveryCtx *ctx);

/* 握手完成后清理对应 PN 空间。
 * is_server=1：只清 Initial 空间，Handshake 空间保留到收到首个 1-RTT 包
 *              （由 quic_recovery_clear_handshake 清理）。
 * is_server=0：Initial + Handshake 都清。 */
void quic_recovery_handshake_done(QuicRecoveryCtx *ctx, int is_server);
void quic_recovery_clear_handshake(QuicRecoveryCtx *ctx);

uint64_t quic_recovery_get_srtt(const QuicRecoveryCtx *ctx);
uint64_t quic_recovery_get_bytes_in_flight(const QuicRecoveryCtx *ctx);
void     quic_recovery_reset_pto(QuicRecoveryCtx *ctx);
int      quic_recovery_can_send(const QuicRecoveryCtx *ctx);

#ifdef __cplusplus
}
#endif
#endif
