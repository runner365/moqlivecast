#include "quic_connection.h"
#include "quic_packet.h"
#include "quic_crypto.h"
#include "quic_stream.h"
#include "quic_recovery.h"
#include "quic_timer.h"
#include "tls_common.h"
#include "logger.h"
#include <openssl/core_dispatch.h>
#include <openssl/rand.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>

#define TLS_BUF_SIZE 65536

/* ============================================
 * 结构体
 * ============================================ */
struct QuicConnection {
    uv_loop_t *loop_;
    QuicState  state_;
    int        uv_close_left_;  /* 尚未 close 完的 uv handle 数 */
    int        freeing_;        /* 已进入延迟释放 */

    /* 连接标识 */
    QuicConnectionId src_cid_, dst_cid_;   /* 我方 / 对端 */
    uint64_t dst_cid_seq_;                 /* 当前 dst_cid 的 sequence number */
    uint64_t retire_prior_to_;             /* NEW_CONNECTION_ID 要求 retire 的上限 */
    char peer_ip_[64];
    int  peer_port_;

    /* UDP */
    uv_udp_t *udp_;
    int       udp_shared_;     /* 服务端共享 listener 的 udp */
    int       ssl_ctx_shared_; /* SSL_CTX 来自 listener，不要在析构时释放 */
    uint8_t  *udp_rbuf_;       /* recv buffer */
    size_t    udp_rbuf_sz_;

    /* TLS */
    SSL_CTX  *ssl_ctx_;
    SSL      *ssl_;
    uint32_t  read_level_;
    uint32_t  last_recv_level_;  /* crypto_recv_rcd 保存，防止 yield_secret 中途修改 */
    uint32_t  write_level_;    /* 用于判断出站包类型 */
    int       handshake_done_;
    int       crypto_send_pending_;  /* crypto_send_cb 因反放大未发完，需重试 */

    /* CRYPTO 流发送偏移 — 每个加密级别独立 */
    uint64_t  crypto_send_off_[QUIC_TLS_LEVEL_NUM];

    /* CRYPTO 发送缓存（重传由 recovery PTO 精确重传 chunk 接管，此缓存保留） */
    uint8_t  *crypto_sent_buf_[QUIC_TLS_LEVEL_NUM];
    size_t    crypto_sent_len_[QUIC_TLS_LEVEL_NUM];

    /* 每级 TLS 接收缓冲 */
    uint8_t *tls_rbuf_[QUIC_TLS_LEVEL_NUM];
    uint8_t *tls_fill_[QUIC_TLS_LEVEL_NUM];  /* bitmap: 1=真实数据, 0=gap零填充 */
    size_t   tls_rlen_[QUIC_TLS_LEVEL_NUM];    /* 缓冲区中待消费字节数 */
    size_t   tls_rcap_[QUIC_TLS_LEVEL_NUM];
    size_t   tls_consumed_[QUIC_TLS_LEVEL_NUM]; /* 已被 OpenSSL 消费的字节数 */
    size_t   tls_contig_[QUIC_TLS_LEVEL_NUM];      /* 连续无 gap 的最大偏移 */
    size_t   tls_contig_fed_[QUIC_TLS_LEVEL_NUM];  /* 上次投喂 SSL_accept 时的 contig */

    /* 包保护密钥 — [level][0=read,1=write] */
    QuicCipherKeys keys_[QUIC_TLS_LEVEL_NUM][2];

    /* Packet number — 每个保护级别独立 */
    uint64_t next_pn_[QUIC_TLS_LEVEL_NUM];
    uint64_t largest_rx_pn_[QUIC_TLS_LEVEL_NUM];

    /* AEAD 包计数 & 限值 (RFC 9001 §6.6) — 单密钥最多加密 ~2^24 包 */
    uint64_t aead_pkt_count_[QUIC_TLS_LEVEL_NUM];
    int      aead_limit_warned_[QUIC_TLS_LEVEL_NUM];
    #define QUIC_AEAD_LIMIT_WARN   (1ULL << 20)  /* ~1M → 提前告警 */
    #define QUIC_AEAD_LIMIT_HARD   (1ULL << 24)  /* 硬上限 → 必须 key update */

    /* Key Phase 跟踪 (RFC 9001 §6) */
    uint8_t   peer_key_phase_;    /* 对端当前 key phase (0 or 1) */
    uint8_t   current_key_phase_; /* 我方当前 key phase (0 or 1) */
    uint8_t   spin_bit_;          /* Spin Bit (RFC 9000 §17.3.1): 回显对端最新值 */
    uint8_t   app_read_secret_[32];   /* 保存的 1-RTT read secret */
    uint8_t   app_write_secret_[32];  /* 保存的 1-RTT write secret */
    int       app_read_secret_saved_;  /* 已保存 read secret */
    int       app_write_secret_saved_; /* 已保存 write secret */
    uint64_t  pkts_sent_with_key_;     /* 当前 key phase 已发送包数 */
    uint64_t  pkts_acked_with_key_;    /* 当前 key phase 已被确认的包 PN 上限 */
#define QUIC_KEY_UPDATE_INTERVAL   5000  /* 每 5000 包发起一次 Key Update */

    /* 收到的 PN — 每级 PN 空间独立跟踪 ACK */
    int      need_ack_[QUIC_TLS_LEVEL_NUM];
    uint64_t ack_largest_pn_[QUIC_TLS_LEVEL_NUM];
    uint64_t ack_first_range_[QUIC_TLS_LEVEL_NUM];

    /* 关闭 */
    uint64_t   close_ec_;
    char       close_reason_[256];
    QuicTimer  close_timer_;

    /* 回调 */
    QuicConnectionOnConnected on_connected_;
    QuicConnectionOnClose     on_close_;

    /* Stream 层 */
    QuicStreamCtx stream_ctx_;

    /* 恢复层 */
    QuicRecoveryCtx recovery_ctx_;

    /* Anti-amplification (RFC 9000 §8.1): 地址验证前限制 3x */
    uint64_t bytes_recv_addr_val_;  /* 本连接已收到的字节数 */
    uint64_t bytes_sent_addr_val_;  /* 本连接已发送的字节数 */
    int      peer_address_validated_; /* 收到客户端有效 Handshake 包后置位，解除 3x 限制 */

    /* Stateless Reset tokens (RFC 9000 §10.3) */
    #define QUIC_MAX_RESET_TOKENS  4
    uint8_t reset_tokens_[QUIC_MAX_RESET_TOKENS][16];
    int     reset_token_count_;

    /* Listener 反向引用 + 应用层数据 */
    void *listener_;
    void *app_data_;

    /* 本地 transport parameters（保持活跃至握手完成） */
    uint8_t    local_tp_[512];
    size_t     local_tp_len_;

    /* 帧打包 */
    uint8_t    pkt_payload_[QUIC_MAX_PKT_SIZE];
    size_t     pkt_payload_len_;
    QuicTimer  flush_timer_;
    int        flush_backoff_ms_;   /* cwnd 拥塞时的重试退避 */
    uint64_t   last_cwnd_block_log_ms_;
#define QUIC_FLUSH_BACKOFF_MAX_MS 50

    /* 空闲超时（RFC 9000 §10.1） */
    uint64_t   idle_timeout_ms_;
    QuicTimer  idle_timer_;
#define QUIC_IDLE_TIMEOUT_DEFAULT_MS 20000

    /* 保活（PING at idle_timeout/2） */
    int        keepalive_enabled_;
    QuicTimer  keepalive_timer_;

    /* DPLPMTUD (RFC 8899) — Path MTU Discovery */
    QuicTimer  pmtu_timer_;
    uint64_t   current_pmtu_;      /* 当前路径 MTU */
    uint64_t   probe_size_;        /* 下一次探测包大小（0=不探测中） */
    uint64_t   probe_pn_;          /* 探测包 PN（用于确认追踪） */

    /* 延缓握手标记 — 1=等所有coalesced数据到齐后 CryptoFlushHandshake 触发 */
    int      handshake_deferred_;

    /* 服务端 Handshake 空间是否已清理（首个 1-RTT 包到达时清理一次） */
    int      handshake_space_cleared_;

    /* WT session: 暂停恢复层包追踪，防止 CONNECT stream PTO 超时 */
    int      recovery_paused_;

    /* 缓存在 handshake read keys 就绪前到达的 Handshake 包 */
    uint8_t *pending_hs_data_;
    size_t   pending_hs_len_;
    size_t   pending_hs_cap_;

    /* 缓存在 application read keys 就绪前到达的 1-RTT 短头包。
     * 握手完成前对端可能已发 1-RTT 包（如 H3 SETTINGS），
     * 无缓冲会直接丢弃 → SETTINGS 交换死锁。 */
    uint8_t *pending_1rtt_data_;
    size_t   pending_1rtt_len_;
    size_t   pending_1rtt_cap_;

    /* DATAGRAM (RFC 9221 §4) */
    uint64_t    max_datagram_size_peer_;   /* 对端声明的大小，0=不支持 */
    uint64_t    max_datagram_size_local_;  /* 本地声明的大小 */
    QuicConnectionOnDatagram on_datagram_;

    /* 流量统计（UDP 字节 / 报文） */
    uint64_t stats_bytes_sent_;
    uint64_t stats_bytes_recv_;
    uint64_t stats_pkts_sent_;
    uint64_t stats_pkts_recv_;
    uint64_t send_rate_start_ms_;
    uint64_t send_rate_bytes_;
    uint64_t send_kbps_;
    uint64_t recv_rate_start_ms_;
    uint64_t recv_rate_bytes_;
    uint64_t recv_kbps_;
};

/* ============================================
 * 前向声明
 * ============================================ */
static int  send_quic_packet(QuicConnection *conn, int pkt_type,
                             const uint8_t *payload, size_t plen);
static void record_rx_pn(QuicConnection *conn, int level, uint64_t pn);
static int  build_and_send_ack(QuicConnection *conn, int level);
static void arm_idle_timer(QuicConnection *conn);
static void arm_keepalive_timer(QuicConnection *conn);
static void on_pmtu_probe(void *user);
static void on_close_timer(void *user);
static void recovery_conn_dead_cb(void *vconn);
static void maybe_initiate_key_update(QuicConnection *conn);
int  quic_conn_queue_frames(void *vconn, const uint8_t *frames, size_t flen);
void quic_conn_flush_packet(void *vconn);
int  QuicConnectionSendImmediate(void *vconn, int pkt_type,
                                  const uint8_t *data, size_t len);

static void rate_add(uint64_t *start_ms, uint64_t *bytes, uint64_t *last_kbps,
                     uint64_t now_ms, uint64_t nbytes) {
    if (!*start_ms) *start_ms = now_ms;
    if (now_ms < *start_ms) {
        *start_ms = now_ms;
        *bytes = 0;
    }
    uint64_t elapsed = now_ms - *start_ms;
    if (elapsed >= 1000) {
        *last_kbps = (*bytes * 8) / elapsed;
        *start_ms = now_ms;
        *bytes = 0;
    }
    *bytes += nbytes;
}

static uint64_t rate_kbps(uint64_t start_ms, uint64_t bytes,
                          uint64_t last_kbps, uint64_t now_ms) {
    if (!start_ms || now_ms < start_ms) return last_kbps;
    uint64_t elapsed = now_ms - start_ms;
    if (elapsed >= 200 && bytes > 0)
        return (bytes * 8) / elapsed;
    return last_kbps;
}

static void stats_on_send(QuicConnection *conn, size_t len) {
    uint64_t now = uv_now(conn->loop_);
    conn->stats_bytes_sent_ += len;
    conn->stats_pkts_sent_++;
    rate_add(&conn->send_rate_start_ms_, &conn->send_rate_bytes_,
             &conn->send_kbps_, now, (uint64_t)len);
}

static void stats_on_recv(QuicConnection *conn, size_t len) {
    uint64_t now = uv_now(conn->loop_);
    conn->stats_bytes_recv_ += len;
    conn->stats_pkts_recv_++;
    rate_add(&conn->recv_rate_start_ms_, &conn->recv_rate_bytes_,
             &conn->recv_kbps_, now, (uint64_t)len);
}

/* ============================================
 * 帧打包（供 quic_stream.c 通过函数指针调用）
 * ============================================ */

static void quic_conn_retry_send(QuicConnection *conn) {
    quic_stream_flush_pending(&conn->stream_ctx_, conn,
                               quic_conn_queue_frames,
                               quic_conn_flush_packet);
}

static void on_flush_timer(void *user);

/* 应用数据出站必须过 cwnd；绕过会在高 RTT 下突发超发 → 丢包 → BBR PC 重置 */
static void arm_flush_backoff(QuicConnection *conn) {
    if (conn->flush_backoff_ms_ < 1) conn->flush_backoff_ms_ = 1;
    if (conn->flush_backoff_ms_ < QUIC_FLUSH_BACKOFF_MAX_MS)
        conn->flush_backoff_ms_ *= 2;
    if (conn->flush_backoff_ms_ > QUIC_FLUSH_BACKOFF_MAX_MS)
        conn->flush_backoff_ms_ = QUIC_FLUSH_BACKOFF_MAX_MS;
    quic_timer_start(&conn->flush_timer_, on_flush_timer, conn,
                     conn->flush_backoff_ms_, 0);
}

static void log_cwnd_block(QuicConnection *conn, const char *where) {
    uint64_t now = uv_now(conn->loop_);
    if (conn->last_cwnd_block_log_ms_ &&
        now - conn->last_cwnd_block_log_ms_ < 1000) {
        return;
    }
    conn->last_cwnd_block_log_ms_ = now;
    uint64_t cwnd = 0;
    uint64_t bif = quic_recovery_get_bytes_in_flight(&conn->recovery_ctx_);
    if (conn->recovery_ctx_.cc_ && conn->recovery_ctx_.cc_->ops &&
        conn->recovery_ctx_.cc_->ops->get_cwnd) {
        cwnd = conn->recovery_ctx_.cc_->ops->get_cwnd(conn->recovery_ctx_.cc_);
    }
    struct quic_cc_info info = {0};
    if (conn->recovery_ctx_.cc_ && conn->recovery_ctx_.cc_->ops &&
        conn->recovery_ctx_.cc_->ops->get_info) {
        conn->recovery_ctx_.cc_->ops->get_info(conn->recovery_ctx_.cc_, &info);
    }
    LOG_DEBUG("[quic-conn] %s deferred by cwnd bif=%llu cwnd=%llu payload=%zu backoff=%dms, cwnd=%llu max_bw_bps=%llu, state:%d, min_rtt_us=%llu",
             where,
             (unsigned long long)bif, (unsigned long long)cwnd,
             conn->pkt_payload_len_, conn->flush_backoff_ms_,
            info.cwnd, info.max_bw_bps, info.state, info.min_rtt_us);
}

static void on_flush_timer(void *user) {
    QuicConnection *conn = (QuicConnection*)user;
    if (!conn || conn->freeing_ || conn->state_ >= QUIC_STATE_CLOSED) return;

    if (conn->pkt_payload_len_ == 0) {
        /* 流控解开后 send_buf 还在，空 payload 也要再喂一次 */
        quic_conn_retry_send(conn);
        if (conn->pkt_payload_len_ == 0) {
            conn->flush_backoff_ms_ = 1;
            /* send_buf 仍可能被流控挡住：不能丢掉心跳，否则再也没人喂 */
            if (!quic_timer_is_active(&conn->flush_timer_)) {
                for (size_t i = 0; i < conn->stream_ctx_.stream_cnt; i++) {
                    QuicStream *s = conn->stream_ctx_.streams[i];
                    if (s && s->send_buf_len > 0) {
                        quic_timer_start(&conn->flush_timer_, on_flush_timer,
                                         conn, QUIC_TIMER_TICK_MS, 0);
                        break;
                    }
                }
            }
            return;
        }
    }

    if (!quic_recovery_can_send(&conn->recovery_ctx_)) {
        log_cwnd_block(conn, "flush_timer");
        /* bif 卡在 cwnd 之上时主动跑一遍丢包检测，回收 inflight */
        quic_recovery_check_losses(&conn->recovery_ctx_);
        arm_flush_backoff(conn);
        return;
    }

    conn->flush_backoff_ms_ = 1;
    send_quic_packet(conn, -1, conn->pkt_payload_, conn->pkt_payload_len_);
    conn->pkt_payload_len_ = 0;

    /* Packet sent → CWND consumed, but stream send_buf may still
     * have pending data. Feed next chunk. */
    quic_conn_retry_send(conn);
}

int quic_conn_queue_frames(void *vconn, const uint8_t *frames, size_t flen) {
    QuicConnection *conn = (QuicConnection*)vconn;
    /* 单个帧超过 MTU 容量 → 拒绝（上层应拆分为 ≤1300B 的 chunk） */
    if (flen > QUIC_MAX_PAYLOAD_LEN) {
        LOG_ERROR("[quic-conn] single frame too large: %zu > %u, dropped",
                  flen, QUIC_MAX_PAYLOAD_LEN);
        return -1;
    }
    /* 如果装不下，先 flush（必须过 cwnd，否则高 RTT 下无限突发） */
    if (conn->pkt_payload_len_ + flen > QUIC_MAX_PAYLOAD_LEN) {
        if (conn->pkt_payload_len_ > 0) {
            if (!quic_recovery_can_send(&conn->recovery_ctx_)) {
                log_cwnd_block(conn, "queue_frames");
                arm_flush_backoff(conn);
                return -1;
            }
            quic_conn_flush_packet(conn);
            if (conn->pkt_payload_len_ + flen > QUIC_MAX_PAYLOAD_LEN) {
                /* flush 因 cwnd 未真正发出，或仍装不下 */
                arm_flush_backoff(conn);
                return -1;
            }
        }
    }
    memcpy(conn->pkt_payload_ + conn->pkt_payload_len_, frames, flen);
    conn->pkt_payload_len_ += flen;
    quic_timer_start(&conn->flush_timer_, on_flush_timer, conn, 1, 0);
    return 0;
}

void quic_conn_queue_ack(void *vconn) {
    QuicConnection *conn = (QuicConnection*)vconn;
    int level = QUIC_TLS_LEVEL_APPLICATION;
    if (!conn->need_ack_[level]) return;
    conn->need_ack_[level] = 0;

    uint8_t ack_buf[256];
    QuicAckFrame ack;
    memset(&ack, 0, sizeof(ack));
    ack.largest_acknowledged = conn->ack_largest_pn_[level];
    ack.ack_delay            = 0;
    ack.num_ranges           = 0;
    ack.first_ack_range      = conn->ack_first_range_[level];

    int alen = quic_frame_write_ack(ack_buf, sizeof(ack_buf), &ack);
    if (alen < 0) return;

    /* ACK 帧放在最前面 */
    if (conn->pkt_payload_len_ + (size_t)alen > QUIC_MAX_PAYLOAD_LEN) {
        /* 先尝试按 cwnd 发出已有 payload；发不出则仍拼 ACK（优先反馈） */
        if (conn->pkt_payload_len_ > 0 &&
            quic_recovery_can_send(&conn->recovery_ctx_)) {
            send_quic_packet(conn, -1, conn->pkt_payload_, conn->pkt_payload_len_);
            conn->pkt_payload_len_ = 0;
        } else if (conn->pkt_payload_len_ > 0) {
            /* 保留 STREAM payload，本次只发纯 ACK，避免饿死对端 */
            send_quic_packet(conn, -1, ack_buf, (size_t)alen);
            if (!quic_timer_is_active(&conn->flush_timer_)) {
                arm_flush_backoff(conn);
            }
            return;
        }
    }

    memmove(conn->pkt_payload_ + alen, conn->pkt_payload_, conn->pkt_payload_len_);
    memcpy(conn->pkt_payload_, ack_buf, (size_t)alen);
    conn->pkt_payload_len_ += (size_t)alen;
    /* 不立即 flush — 让 flush_timer 自然触发。
     * 若 Echo STREAM 帧在 flush 之前到达 quic_conn_queue_frames，
     * ACK 与 STREAM 可合并入同一包，消除 ACK-only 包导致的 Chrome reader 饥饿。 */
    if (!quic_timer_is_active(&conn->flush_timer_)) {
        quic_timer_start(&conn->flush_timer_, on_flush_timer, conn, 1, 0);
    }
}

void quic_conn_flush_packet(void *vconn) {
    QuicConnection *conn = (QuicConnection*)vconn;
    if (conn->pkt_payload_len_ == 0) {
        if (!quic_timer_is_active(&conn->flush_timer_)) {
            quic_timer_start(&conn->flush_timer_, on_flush_timer, conn, 1, 0);
        }
        return;
    }
    if (!quic_recovery_can_send(&conn->recovery_ctx_)) {
        log_cwnd_block(conn, "flush_packet");
        arm_flush_backoff(conn);
        return; /* 保留 pkt_payload_，等 ACK 释放 cwnd */
    }
    conn->flush_backoff_ms_ = 1;
    quic_timer_stop(&conn->flush_timer_);
    send_quic_packet(conn, -1, conn->pkt_payload_, conn->pkt_payload_len_);
    conn->pkt_payload_len_ = 0;
    /* 只发包，不要在这里 flush_pending。
     * consume_send_buf 在流控满时会 flush_fn → 本函数；若再 refill，
     * 流仍 blocked，会 flush_packet → flush_pending → consume → 递归栈溢出。
     * 未发完的 send_buf 由 flush_timer / ACK 路径再喂。 */
    if (!quic_timer_is_active(&conn->flush_timer_)) {
        quic_timer_start(&conn->flush_timer_, on_flush_timer, conn, 1, 0);
    }
}

/* ============================================
 * TLS Dispatch 回调
 * ============================================ */

static int crypto_send_cb(SSL *s, const unsigned char *buf, size_t buf_len,
                          size_t *consumed, void *arg) {
    QuicConnection *conn = (QuicConnection*)arg;
    (void)s;

    /* ── 确定目标保护级别 / packet type ──
     * 用 write_level_（yield_secret_cb 记录的 OpenSSL 当前写级别），
     * 而不是 keys_[...][1].initialized（「密钥已派生」的间接推断）。
     * 后者在大证书链分片发送时，application 密钥可能在 Handshake 数据
     * 还没发完就派生，导致后续分片误用短头 → send packet failed。 */
    int pkt_type, level;
    LOG_DEBUG("[quic-conn] crypto_send: write_level=%u buf_len=%zu app_key=%d hs_key=%d",
             (unsigned)conn->write_level_, buf_len,
             conn->keys_[QUIC_TLS_LEVEL_APPLICATION][1].initialized,
             conn->keys_[QUIC_TLS_LEVEL_HANDSHAKE][1].initialized);
    /* dump 握手消息类型序列（每个 TLS 握手消息第1字节是 msg_type）*/
    {
        char hex[256]; size_t hp = 0;
        for (size_t i = 0; i < buf_len && i < 200 && hp < sizeof(hex)-3; i++)
            hp += snprintf(hex+hp, sizeof(hex)-hp, "%02x", buf[i]);
        LOG_DEBUG("[quic-conn] crypto_send head[200]: %s", hex);
    }
    if (conn->write_level_ == QUIC_TLS_LEVEL_APPLICATION) {
        pkt_type = -1;
        level = QUIC_TLS_LEVEL_APPLICATION;
    } else if (conn->write_level_ == QUIC_TLS_LEVEL_HANDSHAKE) {
        pkt_type = QUIC_PKT_HANDSHAKE;
        level = QUIC_TLS_LEVEL_HANDSHAKE;
    } else {
        /* Initial keys only — 全部以 Initial 级别发送。
         * 与 quic-go 行为一致，避免 ngtcp2
         * negotiated_version 未设置导致 Handshake 包被丢弃。 */
        pkt_type = QUIC_PKT_INITIAL;
        level = QUIC_TLS_LEVEL_NONE;
    }

    uint64_t crypto_off = conn->crypto_send_off_[level];
    size_t   sent_total = 0;
    LOG_DEBUG("[quic-conn] crypto_send_cb: level=%d crypto_off=%llu buf_len=%zu",
             level, (unsigned long long)crypto_off, buf_len);

    while (sent_total < buf_len) {
        size_t chunk = buf_len - sent_total;
        if (chunk > QUIC_INITIAL_MAX_PAYLOAD) chunk = QUIC_INITIAL_MAX_PAYLOAD;

        uint8_t frame[2048];
        int flen = quic_frame_write_crypto(frame, sizeof(frame),
                                            crypto_off + sent_total,
                                            buf + sent_total, chunk);
        if (flen < 0) {
            LOG_ERROR("[quic-conn] crypto frame encode failed: off=%llu len=%zu",
                      (unsigned long long)(crypto_off + sent_total), chunk);
            *consumed = sent_total;
            return 0;
        }

        int ret = send_quic_packet(conn, pkt_type, frame, (size_t)flen);
        if (ret < 0) {
            LOG_WARN("[quic-conn] send packet failed (anti-amp): off=%llu len=%zu, retry whole buf",
                     (unsigned long long)(crypto_off + sent_total), chunk);
            /* 反放大预算不足 → 整个 buf 不消费，返回 0，并置 pending 标志。
             * CryptoFlushHandshake 看到 pending 会重新调 SSL_accept 触发重发。
             * 已发的分片会重复发，但 CRYPTO 帧带 offset，对端幂等去重。 */
            conn->crypto_send_pending_ = 1;
            *consumed = 0;
            return 0;
        }

        sent_total += chunk;
    }

    /* 缓存发送数据（追加模式）— 丢包时可重发整个 CRYPTO 流 */
    {
        size_t new_len = (size_t)crypto_off + buf_len;
        uint8_t *nb = (uint8_t*)realloc(conn->crypto_sent_buf_[level], new_len);
        if (nb) {
            conn->crypto_sent_buf_[level] = nb;
            memcpy(nb + crypto_off, buf, buf_len);  /* 追加到正确偏移 */
            conn->crypto_sent_len_[level] = new_len;
        }
    }

    conn->crypto_send_off_[level] += buf_len;
    *consumed = buf_len;

    LOG_DEBUG("[quic-conn] crypto_send: %zu bytes in %zu pkts (level=%d pkt_type=%d)",
              buf_len, (buf_len + QUIC_INITIAL_MAX_PAYLOAD - 1) / QUIC_INITIAL_MAX_PAYLOAD, level, pkt_type);
    return 1;
}

static int crypto_recv_rcd_cb(SSL *s, const unsigned char **buf,
                               size_t *bytes_read, void *arg) {
    QuicConnection *conn = (QuicConnection*)arg;
    (void)s;

    int level = (int)conn->read_level_;
    conn->last_recv_level_ = level;   /* 保存到 release 回调使用，防止 yield_secret 中途改 read_level_ */

    if (conn->tls_rlen_[level] == 0) {
        *buf = NULL;
        *bytes_read = 0;
        return 1;
    }

    /* 限制 OpenSSL 只能读取连续数据（无 gap 零填充）。
     * contig_ 和 consumed_ 是绝对 CRYPTO stream offset，rlen_ 是缓冲区相对长度。
     * 可安全读取的连续字节 = contig_ - consumed_（已消费的不重复读）。 */
    size_t avail = conn->tls_rlen_[level];
    if (conn->tls_contig_[level] > conn->tls_consumed_[level]) {
        size_t contig_avail = conn->tls_contig_[level] - conn->tls_consumed_[level];
        if (contig_avail < avail)
            avail = contig_avail;
    } else {
        avail = 0;
    }

    LOG_DEBUG("[quic-conn] crypto_recv_rcd: level=%d buf=%p avail=%zu rlen=%zu contig=%zu consumed=%zu",
             level, (void*)conn->tls_rbuf_[level], avail, conn->tls_rlen_[level],
             conn->tls_contig_[level], conn->tls_consumed_[level]);

    *buf = conn->tls_rbuf_[level];
    *bytes_read = avail;
    return 1;
}

static int crypto_release_rcd_cb(SSL *s, size_t bytes_read, void *arg) {
    QuicConnection *conn = (QuicConnection*)arg;
    (void)s;

    int level = (int)conn->last_recv_level_;  /* 使用 recv 时保存的 level，防止 yield_secret 改写 */
    if (bytes_read > 0 && bytes_read <= conn->tls_rlen_[level]) {
        size_t rem = conn->tls_rlen_[level] - bytes_read;
        if (rem > 0) {
            memmove(conn->tls_rbuf_[level],
                    conn->tls_rbuf_[level] + bytes_read, rem);
            /* shift fill bitmap: memmove(bm, bm+bytes_read/8, ...) + bit shift */
            size_t bm_bytes = (TLS_BUF_SIZE + 7) / 8;
            size_t shift_b  = bytes_read / 8;
            size_t shift_r  = bytes_read % 8;
            if (shift_b > 0 && shift_b < bm_bytes) {
                memmove(conn->tls_fill_[level],
                        conn->tls_fill_[level] + shift_b,
                        bm_bytes - shift_b);
                memset(conn->tls_fill_[level] + bm_bytes - shift_b, 0, shift_b);
            }
            if (shift_r > 0) {
                for (size_t i = 0; i + 1 < bm_bytes; i++)
                    conn->tls_fill_[level][i] =
                        (uint8_t)((conn->tls_fill_[level][i] >> shift_r) |
                                  (conn->tls_fill_[level][i+1] << (8 - shift_r)));
                conn->tls_fill_[level][bm_bytes - 1] >>= shift_r;
            }
        } else {
            /* rem==0: buffer 全消费。bitmap 必须清零，否则残留已消费
             * 数据的标记，后续 gap 分支的 contig 扫描会误推进，
             * 导致 OpenSSL 消费 gap 零填充的假数据（TLS alert code=10）。 */
            memset(conn->tls_fill_[level], 0, (TLS_BUF_SIZE + 7) / 8);
        }
        conn->tls_rlen_[level] = rem;
        conn->tls_consumed_[level] += bytes_read;
    }
    LOG_DEBUG("[quic-conn] crypto_release_rcd: level=%d, released %zu, rem=%zu, consumed=%zu"
             " contig=%zu",
           level, bytes_read, conn->tls_rlen_[level],
           conn->tls_consumed_[level], conn->tls_contig_[level]);
    return 1;
}

static int yield_secret_cb(SSL *s, uint32_t prot_level, int direction,
                            const unsigned char *secret, size_t secret_len,
                            void *arg) {
    QuicConnection *conn = (QuicConnection*)arg;
    (void)s;

    LOG_DEBUG("[quic-conn] yield_secret: level=%u dir=%s len=%zu",
           (unsigned)prot_level, direction == 0 ? "read" : "write", secret_len);

    /* Wireshark keylog — write to /tmp/quic_keys_<pid>.txt */
    {
        static FILE *kf = NULL;
        if (!kf) {
            char path[64];
            snprintf(path, sizeof(path), "/tmp/quic_keys_%d.txt", (int)getpid());
            kf = fopen(path, "w");
        }
        if (kf && prot_level >= 2 && prot_level <= 3) {
            unsigned char cr[32];
            size_t cr_len = SSL_get_client_random(conn->ssl_, cr, sizeof(cr));
            const char *label = (direction == 0)
                ? (prot_level == 2 ? "QUIC_CLIENT_HANDSHAKE_TRAFFIC_SECRET"
                                   : "QUIC_CLIENT_TRAFFIC_SECRET_0")
                : (prot_level == 2 ? "QUIC_SERVER_HANDSHAKE_TRAFFIC_SECRET"
                                   : "QUIC_SERVER_TRAFFIC_SECRET_0");
            fprintf(kf, "%s ", label);
            for (size_t i = 0; i < cr_len; i++) fprintf(kf, "%02x", cr[i]);
            fprintf(kf, " ");
            for (size_t i = 0; i < secret_len; i++) fprintf(kf, "%02x", secret[i]);
            fprintf(kf, "\n");
            fflush(kf);
            /* 最后一层（application read）写完关文件 */
            if (prot_level == 3 && direction == 0) {
                fclose(kf);
                kf = NULL;
            }
        }
    }

    /* TLS cipher 在此级别已协商完成——在派生密钥前读取 cipher suite */
    if (prot_level >= QUIC_TLS_LEVEL_HANDSHAKE) {
        quic_crypto_set_cipher_suite_from_ssl(conn->ssl_);
    }

    if (direction == 0) {
        conn->read_level_ = prot_level;
    } else {
        conn->write_level_ = prot_level;
    }

    /* 派生 QUIC 包保护密钥 */
    if (quic_crypto_derive_from_secret(&conn->keys_[prot_level][direction],
                                        secret, secret_len) < 0) {
        LOG_ERROR("[quic-conn] derive keys failed: level=%u dir=%s",
               (unsigned)prot_level, direction == 0 ? "read" : "write");
        return 0;
    }

    /* 握手读密钥就绪 — 标记稍后回放缓存的 Handshake 包。
     * 不能在 yield_secret_cb 内直接调用 QuicConnectionFeedRaw，
     * 因为 yield_secret 是从 SSL_do_handshake 内部触发的，
     * 重入 QuicConnectionFeedRaw→SSL_do_handshake 会导致 OpenSSL 崩溃。
     * 回放推迟到 QuicConnectionFeedRaw 中 SSL_do_handshake 返回后。 */

    /* 保存 1-RTT application secret 用于 Key Update (RFC 9001 §6) */
    if (prot_level == QUIC_TLS_LEVEL_APPLICATION) {
        size_t save_len = secret_len < 32 ? secret_len : 32;
        if (direction == 0) {
            memcpy(conn->app_read_secret_, secret, save_len);
            conn->app_read_secret_saved_ = 1;
            LOG_DEBUG("[quic-conn] saved app read secret[%zu] = %02x%02x..%02x%02x",
                     save_len, secret[0], secret[1], secret[30], secret[31]);
        } else {
            memcpy(conn->app_write_secret_, secret, save_len);
            conn->app_write_secret_saved_ = 1;
            LOG_DEBUG("[quic-conn] saved app write secret[%zu] = %02x%02x..%02x%02x",
                     save_len, secret[0], secret[1], secret[30], secret[31]);
        }
    }

    return 1;
}

static int got_transport_params_cb(SSL *s, const unsigned char *params,
                                    size_t params_len, void *arg) {
    (void)s;
    QuicConnection *conn = (QuicConnection*)arg;
    LOG_DEBUG("[quic-conn] got_transport_params: %zu bytes", params_len);

    /* 解析对端 transport parameters */
    size_t pos = 0;
    while (pos < params_len) {
        uint64_t tp_id, tp_len;
        int v = quic_varint_decode(params + pos, params_len - pos, &tp_id);
        if (v < 0) break;
        pos += v;
        v = quic_varint_decode(params + pos, params_len - pos, &tp_len);
        if (v < 0) break;
        pos += v;
        if (pos + tp_len > params_len) break;

        switch (tp_id) {
        case 0x04: { /* initial_max_data */
            uint64_t md;
            if (quic_varint_decode(params + pos, tp_len, &md) > 0) {
                conn->stream_ctx_.max_data_peer = md;
                LOG_DEBUG("[quic-conn] peer initial_max_data=%llu",
                          (unsigned long long)md);
            }
            break;
        }
        case 0x05: { /* initial_max_stream_data_bidi_local — 对端发起的 bidi */
            uint64_t md;
            if (quic_varint_decode(params + pos, tp_len, &md) > 0) {
                conn->stream_ctx_.peer_init_max_stream_data_bidi_local = md;
                LOG_DEBUG("[quic-conn] peer init_max_stream_data_bidi_local=%llu",
                          (unsigned long long)md);
            }
            break;
        }
        case 0x06: { /* initial_max_stream_data_bidi_remote — 我方发起的 bidi */
            uint64_t md;
            if (quic_varint_decode(params + pos, tp_len, &md) > 0) {
                conn->stream_ctx_.peer_init_max_stream_data_bidi_remote = md;
                LOG_DEBUG("[quic-conn] peer init_max_stream_data_bidi_remote=%llu",
                          (unsigned long long)md);
            }
            break;
        }
        case 0x07: { /* initial_max_stream_data_uni */
            uint64_t md;
            if (quic_varint_decode(params + pos, tp_len, &md) > 0) {
                conn->stream_ctx_.peer_init_max_stream_data_uni = md;
                LOG_DEBUG("[quic-conn] peer init_max_stream_data_uni=%llu",
                          (unsigned long long)md);
            }
            break;
        }
        case 0x08: { /* initial_max_streams_bidi */
            uint64_t ms;
            if (quic_varint_decode(params + pos, tp_len, &ms) > 0) {
                conn->stream_ctx_.max_streams_bidi_peer = ms;
                LOG_DEBUG("[quic-conn] peer initial_max_streams_bidi=%llu",
                          (unsigned long long)ms);
            }
            break;
        }
        case 0x09: { /* initial_max_streams_uni */
            uint64_t ms;
            if (quic_varint_decode(params + pos, tp_len, &ms) > 0) {
                conn->stream_ctx_.max_streams_uni_peer = ms;
                LOG_DEBUG("[quic-conn] peer initial_max_streams_uni=%llu",
                          (unsigned long long)ms);
            }
            break;
        }
        case 0x01: { /* max_idle_timeout — RFC 9000 §10.1: min(ours, theirs) */
            uint64_t peer_timeout;
            if (quic_varint_decode(params + pos, tp_len, &peer_timeout) > 0) {
                /* 取较小值作为实际空闲超时 */
                if (peer_timeout > 0 && peer_timeout < conn->idle_timeout_ms_) {
                    conn->idle_timeout_ms_ = peer_timeout;
                    LOG_DEBUG("[quic-conn] negotiated max_idle_timeout=%llums",
                              (unsigned long long)peer_timeout);
                }
            }
            break;
        }
        case 0x0c: /* disable_active_migration — 对端也不支持迁移，只记录 */
            LOG_DEBUG("[quic-conn] peer disabled active migration");
            break;
        case 0x20: { /* max_datagram_frame_size (RFC 9221 §3) */
            uint64_t mdfs;
            if (quic_varint_decode(params + pos, tp_len, &mdfs) > 0) {
                conn->max_datagram_size_peer_ = mdfs;
                LOG_DEBUG("[quic-conn] peer max_datagram_frame_size=%llu",
                         (unsigned long long)mdfs);
            }
            break;
        }
        default:
            break;
        }
        pos += tp_len;
    }
    quic_stream_apply_init_send_credit(&conn->stream_ctx_);
    return 1;
}

static int alert_cb(SSL *s, unsigned char alert_code, void *arg) {
    (void)s; (void)arg;
    LOG_WARN("[quic-conn] TLS alert: code=%u", alert_code);
    return 1;
}

static OSSL_DISPATCH qtdis[] = {
    {OSSL_FUNC_SSL_QUIC_TLS_CRYPTO_SEND,         (void (*)(void))crypto_send_cb},
    {OSSL_FUNC_SSL_QUIC_TLS_CRYPTO_RECV_RCD,     (void (*)(void))crypto_recv_rcd_cb},
    {OSSL_FUNC_SSL_QUIC_TLS_CRYPTO_RELEASE_RCD,  (void (*)(void))crypto_release_rcd_cb},
    {OSSL_FUNC_SSL_QUIC_TLS_YIELD_SECRET,        (void (*)(void))yield_secret_cb},
    {OSSL_FUNC_SSL_QUIC_TLS_GOT_TRANSPORT_PARAMS,(void (*)(void))got_transport_params_cb},
    {OSSL_FUNC_SSL_QUIC_TLS_ALERT,               (void (*)(void))alert_cb},
    {0, NULL}
};

/* ============================================
 * UDP I/O（客户端侧）
 * ============================================ */

typedef struct {
    uv_udp_send_t     req;
    QuicConnection   *conn;
    size_t            data_len;
    char              data[];
} SendCtx;

static void on_send_done(uv_udp_send_t *req, int status) {
    SendCtx *ctx = (SendCtx*)req;
    if (status < 0) {
        LOG_ERROR("[quic-conn] udp send error: %s", uv_strerror(status));
    } else {
        LOG_DEBUG("[quic-conn] udp send done: %zu bytes, status=%d", ctx->data_len, status);
    }
    free(ctx);
}

static void on_udp_alloc(uv_handle_t *h, size_t suggested, uv_buf_t *buf) {
    QuicConnection *conn = (QuicConnection*)h->data;
    buf->base = (char*)conn->udp_rbuf_;
    buf->len  = (unsigned int)conn->udp_rbuf_sz_;
}

static void on_udp_recv(uv_udp_t *h, ssize_t nread, const uv_buf_t *buf,
                        const struct sockaddr *addr, unsigned flags) {
    QuicConnection *conn = (QuicConnection*)h->data;
    if (nread <= 0) {
        if (nread < 0) LOG_ERROR("[quic-conn] udp recv error: %s", uv_strerror((int)nread));
        return;
    }
    LOG_DEBUG("[quic-conn] udp recv: %zd bytes", nread);

    char ip[64];
    int port = 0;
    if (addr->sa_family == AF_INET) {
        struct sockaddr_in *in = (struct sockaddr_in*)addr;
        uv_ip4_name(in, ip, sizeof(ip));
        port = ntohs(in->sin_port);
    } else if (addr->sa_family == AF_INET6) {
        struct sockaddr_in6 *in6 = (struct sockaddr_in6*)addr;
        uv_ip6_name(in6, ip, sizeof(ip));
        port = ntohs(in6->sin6_port);
    }

    const uint8_t *udata = (const uint8_t*)buf->base;
    size_t ulen = (size_t)nread;

    /* Retry 包检测 (RFC 9000 §17.2.5) — 客户端收到后重发 Initial */
    if (ulen >= 20 &&
        (udata[0] & 0xF0) == 0xF0 &&  /* 长头 + Retry type (0xF0) */
        conn->state_ == QUIC_STATE_HANDSHAKE) {
        size_t rp = 1;
        uint32_t ver_n;
        memcpy(&ver_n, udata + rp, 4); rp += 4;
        if (ntohl(ver_n) == QUIC_VERSION_V1) {
            uint8_t dcil = udata[rp++];
            if (rp + dcil <= ulen) {
                QuicConnectionId retry_scid;
                memset(&retry_scid, 0, sizeof(retry_scid));
                retry_scid.len = dcil;
                memcpy(retry_scid.data, udata + rp, dcil);
                rp += dcil;

                uint8_t scil = udata[rp++];
                const uint8_t *retry_token = NULL;
                size_t retry_token_len = 0;
                if (rp + scil + 16 <= ulen) {
                    retry_token = udata + rp + scil;
                    retry_token_len = ulen - rp - scil - 16;
                }

                if (retry_token && retry_token_len > 0) {
                    LOG_DEBUG("[quic-conn] received Retry: new_dcid=%02x%02x.. token=%zu",
                             retry_scid.data[0], retry_scid.data[1],
                             retry_token_len);
                    /* 更新 DCID → 重新派生 Initial 密钥 */
                    quic_cid_copy(&conn->dst_cid_, &retry_scid);
                    /* 密钥已变更，需要重置握手状态。简化：
                     * 直接让 SSL_connect 重新触发，crypto_send_cb 会用
                     * 新的 dst_cid_ 和 derived keys。 */
                    conn->crypto_send_off_[QUIC_TLS_LEVEL_NONE] = 0;
                    SSL_connect(conn->ssl_);
                    return;
                }
            }
        }
    }

    QuicConnectionFeedRaw(conn, udata, ulen, ip, port);
}

static void conn_free_mem(QuicConnection *conn) {
    if (!conn) return;
    if (conn->ssl_) SSL_free(conn->ssl_);
    conn->ssl_ = NULL;
    if (conn->ssl_ctx_ && !conn->ssl_ctx_shared_) tls_ctx_free(conn->ssl_ctx_);
    conn->ssl_ctx_ = NULL;
    for (int i = 0; i < QUIC_TLS_LEVEL_NUM; i++) {
        free(conn->tls_rbuf_[i]); conn->tls_rbuf_[i] = NULL;
        free(conn->tls_fill_[i]); conn->tls_fill_[i] = NULL;
        free(conn->crypto_sent_buf_[i]); conn->crypto_sent_buf_[i] = NULL;
    }
    free(conn->pending_hs_data_);
    conn->pending_hs_data_ = NULL;
    free(conn->pending_1rtt_data_);
    conn->pending_1rtt_data_ = NULL;
    free(conn->udp_rbuf_);
    conn->udp_rbuf_ = NULL;
    if (!conn->udp_shared_) {
        free(conn->udp_);
        conn->udp_ = NULL;
    }
    free(conn);
}

static void conn_free_mem_cb(void *p) {
    conn_free_mem((QuicConnection *)p);
}

static void conn_on_uv_closed(uv_handle_t *h) {
    QuicConnection *conn = (QuicConnection *)h->data;
    if (!conn) return;
    if (--conn->uv_close_left_ > 0) return;
    quic_timer_defer_free(conn, conn_free_mem_cb);
}

static void conn_close_handle(uv_handle_t *h, QuicConnection *conn) {
    if (!h || !h->loop) return;
    if (uv_is_closing(h)) return;
    h->data = conn;
    conn->uv_close_left_++;
    uv_close(h, conn_on_uv_closed);
}

static void on_udp_close(uv_handle_t *h) {
    conn_on_uv_closed(h);
}

/* ============================================
 * 发送
 * ============================================ */

static void do_udp_send(QuicConnection *conn, const uint8_t *data, size_t len) {
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((unsigned short)conn->peer_port_);
    uv_ip4_addr(conn->peer_ip_, conn->peer_port_, &addr);

    SendCtx *ctx = (SendCtx*)malloc(sizeof(SendCtx) + len);
    if (!ctx) {
        LOG_ERROR("[quic-conn] do_udp_send: malloc(%zu) failed", sizeof(SendCtx) + len);
        return;
    }

    ctx->conn      = conn;
    ctx->data_len  = len;
    memcpy(ctx->data, data, len);

    uv_buf_t ubuf = uv_buf_init(ctx->data, (unsigned int)len);
    LOG_DEBUG("[quic-conn] do_udp_send: %zu bytes -> %s:%d",
             len, conn->peer_ip_, conn->peer_port_);
    int ret = uv_udp_send(&ctx->req, conn->udp_, &ubuf, 1,
                          (const struct sockaddr*)&addr, on_send_done);
    if (ret < 0) {
        LOG_ERROR("[quic-conn] uv_udp_send failed: %s", uv_strerror(ret));
        free(ctx);
        return;
    }
    stats_on_send(conn, len);
}

/* 判断 payload 是否包含 ack-eliciting 数据帧（STREAM/CRYPTO）。
 * 关键：必须正确跳过所有「非 ack-eliciting」帧（ACK/PING/PADDING/
 * MAX_DATA/MAX_STREAM_DATA/MAX_STREAMS/BLOCKED 等），继续遍历到
 * 后面的 STREAM/CRYPTO 帧。否则「MAX_DATA+STREAM」合并包（服务端
 * 握手完成时 flush 出的 SETTINGS 包）会被 MAX_DATA 开头误判为
 * ack_eliciting=0 → 丢包后不重传 → SETTINGS 交换死锁。 */
static int payload_ack_eliciting(const uint8_t *payload, size_t plen) {
    size_t pos = 0;
    while (pos < plen) {
        uint8_t ft = payload[pos];
        int c = 0;

        switch (ft) {
        case QUIC_FRAME_PADDING:
        case QUIC_FRAME_PING:
        case QUIC_FRAME_HANDSHAKE_DONE:
            pos += 1;
            continue;

        case QUIC_FRAME_ACK:
        case QUIC_FRAME_ACK_ECN: {
            QuicAckFrame ack;
            c = quic_frame_parse_ack(payload + pos, plen - pos, &ack);
            break;
        }
        case QUIC_FRAME_CRYPTO: {
            uint64_t off; const uint8_t *d; size_t l;
            c = quic_frame_parse_crypto(payload + pos, plen - pos, &off, &d, &l);
            /* CRYPTO 数据帧是 ack-eliciting */
            return (c > 0) ? 1 : 0;
        }
        case QUIC_FRAME_MAX_DATA: {
            uint64_t v;
            c = quic_frame_parse_max_data(payload + pos, plen - pos, &v);
            break;
        }
        case QUIC_FRAME_MAX_STREAM_DATA: {
            uint64_t sid, v;
            c = quic_frame_parse_max_stream_data(payload + pos, plen - pos, &sid, &v);
            break;
        }
        case QUIC_FRAME_MAX_STREAMS_BIDI:
        case QUIC_FRAME_MAX_STREAMS_UNI: {
            uint64_t v; int bidi;
            c = quic_frame_parse_max_streams(payload + pos, plen - pos, &v, &bidi);
            break;
        }
        case QUIC_FRAME_DATA_BLOCKED: {
            uint64_t v;
            c = quic_frame_parse_data_blocked(payload + pos, plen - pos, &v);
            break;
        }
        case QUIC_FRAME_STREAM_DATA_BLOCKED: {
            uint64_t sid, v;
            c = quic_frame_parse_stream_data_blocked(payload + pos, plen - pos, &sid, &v);
            break;
        }
        case QUIC_FRAME_STREAMS_BLOCKED_BIDI:
        case QUIC_FRAME_STREAMS_BLOCKED_UNI: {
            uint64_t v; int bidi;
            c = quic_frame_parse_streams_blocked(payload + pos, plen - pos, &v, &bidi);
            break;
        }
        case QUIC_FRAME_RESET_STREAM: {
            uint64_t sid, ec, fs;
            c = quic_frame_parse_reset_stream(payload + pos, plen - pos, &sid, &ec, &fs);
            break;
        }
        case QUIC_FRAME_STOP_SENDING: {
            uint64_t sid, ec;
            c = quic_frame_parse_stop_sending(payload + pos, plen - pos, &sid, &ec);
            break;
        }
        case QUIC_FRAME_NEW_CONNECTION_ID: {
            uint64_t seq, rp; uint8_t cl; const uint8_t *cd; const uint8_t *rt;
            c = quic_frame_parse_new_conn_id(payload + pos, plen - pos, &seq, &rp, &cl, &cd, &rt);
            break;
        }
        case QUIC_FRAME_RETIRE_CONNECTION_ID: {
            uint64_t seq;
            c = quic_frame_parse_retire_conn_id(payload + pos, plen - pos, &seq);
            break;
        }
        case QUIC_FRAME_NEW_TOKEN: {
            c = quic_frame_parse_new_token(payload + pos, plen - pos);
            break;
        }
        case QUIC_FRAME_CONNECTION_CLOSE:
        case QUIC_FRAME_CONNECTION_CLOSE_APP: {
            uint64_t ec; const char *rs; size_t rl;
            c = quic_frame_parse_connection_close(payload + pos, plen - pos, &ec, &rs, &rl);
            break;
        }
        case QUIC_FRAME_DATAGRAM:
        case QUIC_FRAME_DATAGRAM_LEN: {
            const uint8_t *d; size_t l;
            c = quic_frame_parse_datagram(payload + pos, plen - pos, &d, &l);
            break;
        }
        default:
            /* STREAM 帧类型 0x08..0x0f */
            if (ft >= 0x08 && ft <= 0x0f) return 1;
            /* 未知帧类型 — 保守视为不 ack-eliciting，停止扫描 */
            return 0;
        }

        if (c <= 0) return 0;   /* 解析失败，保守判定 */
        pos += (size_t)c;
    }
    return 0;  /* 全是 ACK/PING/MAX_DATA 等非 ack-eliciting 帧 */
}

static int send_quic_packet(QuicConnection *conn, int pkt_type,
                            const uint8_t *payload, size_t plen) {
    uint8_t pkt[QUIC_MAX_PKT_SIZE];
    size_t pkt_len = 0;
    int ret;

    int level = QUIC_TLS_LEVEL_NONE;
    if (pkt_type == QUIC_PKT_INITIAL) {
        level = QUIC_TLS_LEVEL_NONE;
    } else if (pkt_type == QUIC_PKT_HANDSHAKE) {
        level = QUIC_TLS_LEVEL_HANDSHAKE;
    } else {
        level = QUIC_TLS_LEVEL_APPLICATION;
    }

    uint64_t pn = conn->next_pn_[level]++;

    /* AEAD 包计数 (RFC 9001 §6.6) — 单密钥加密包数接近上限时告警 */
    if (pkt_type == QUIC_PKT_INITIAL || pkt_type == QUIC_PKT_HANDSHAKE ||
        pkt_type == -1) {
        conn->aead_pkt_count_[level]++;
        if (!conn->aead_limit_warned_[level] &&
            conn->aead_pkt_count_[level] >= QUIC_AEAD_LIMIT_WARN) {
            LOG_WARN("[quic-conn] AEAD pkt count=%llu at level=%d"
                     " (limit %llu), Key Update required",
                     (unsigned long long)conn->aead_pkt_count_[level], level,
                     (unsigned long long)QUIC_AEAD_LIMIT_HARD);
            conn->aead_limit_warned_[level] = 1;
        }
    }

    if (pkt_type == QUIC_PKT_INITIAL || pkt_type == QUIC_PKT_HANDSHAKE) {
        QuicCipherKeys *keys = &conn->keys_[level][1]; /* write keys */
        if (!keys->initialized) {
            /* 使用 initial keys */
            keys = (pkt_type == QUIC_PKT_INITIAL)
                 ? &conn->keys_[QUIC_TLS_LEVEL_NONE][1]
                 : NULL;
            if (!keys || !keys->initialized) {
                LOG_ERROR("[quic-conn] send_quic_packet: no write keys for pkt_type=%d level=%d",
                          pkt_type, level);
                return -1;
            }
        }
        ret = quic_packet_build_long(pkt, &pkt_len, pkt_type,
                                      &conn->src_cid_, &conn->dst_cid_,
                                      NULL, 0, pn, payload, plen, keys);
    } else {
        /* 短头 */
        QuicCipherKeys *keys = &conn->keys_[QUIC_TLS_LEVEL_APPLICATION][1];
        if (!keys->initialized) {
            LOG_ERROR("[quic-conn] send_quic_packet: no app write keys for short header, pn=%llu",
                      (unsigned long long)pn);
            return -1;
        }
        ret = quic_packet_build_short(pkt, &pkt_len, &conn->dst_cid_,
                                       pn, payload, plen, keys,
                                       conn->current_key_phase_,
                                       conn->spin_bit_);
    }

    if (ret < 0) {
        LOG_ERROR("[quic-conn] send_quic_packet: build failed, pkt_type=%d pn=%llu plen=%zu",
                  pkt_type, (unsigned long long)pn, plen);
        return -1;
    }

    /* RFC 9000 §8.1: 服务端地址验证前限制 3x 放大。
     * 仅服务端受限制（udp_shared_ = 共享 listener 的 UDP socket）。
     * 所有出站字节（含重传）都计入预算 — 重传同样占线上带宽，
     * 可用于反射放大，RFC 不豁免。
     * 地址验证完成（收到客户端有效 Handshake 包）后解除限制。
     * 豁免：Handshake 级别的 CRYPTO 包（pkt_type==HANDSHAKE）不受限——
     * 正式证书链 ~5KB，超过 3x 预算（客户端 Initial ~1250B → 3750B），
     * 若对握手数据也限制，证书链永远发不完，握手死锁。 */
    if (conn->udp_shared_ && !conn->peer_address_validated_ &&
        pkt_type != QUIC_PKT_HANDSHAKE) {
        uint64_t limit = conn->bytes_recv_addr_val_ * 3;
        if (conn->bytes_sent_addr_val_ + (uint64_t)pkt_len > limit) {
            LOG_WARN("[quic-conn] anti-amplification: drop %zuB (sent=%llu+%zu > 3*%llu=%llu)",
                     pkt_len,
                     (unsigned long long)conn->bytes_sent_addr_val_,
                     pkt_len,
                     (unsigned long long)conn->bytes_recv_addr_val_,
                     (unsigned long long)limit);
            return -1;
        }
        conn->bytes_sent_addr_val_ += (uint64_t)pkt_len;
    }

    do_udp_send(conn, pkt, pkt_len);

    /* PMTU probe: track the PN of 1-RTT packets for ACK tracking */
    if (pkt_type == -1 && conn->probe_size_ > 0) {
        conn->probe_pn_ = pn;
    }

    /* DEBUG: dump the first bytes of the OUTGOING short header packet */
    if (pkt_type == -1) {
        QuicCipherKeys *sk = &conn->keys_[QUIC_TLS_LEVEL_APPLICATION][1];
        char hex[128]; size_t hpos=0;
        for (size_t i=0; i<pkt_len && i<20 && hpos<120; i++)
            hpos += snprintf(hex+hpos, sizeof(hex)-hpos, "%02x", pkt[i]);
        LOG_DEBUG("[quic-conn] sent SHORT: wire[%zu]=%s pn=%llu plen=%zu key=%02x%02x.. iv=%02x%02x..",
                 pkt_len, hex, (unsigned long long)pn, plen,
                 sk->key[0], sk->key[1], sk->iv[0], sk->iv[1]);
    }

    /* 记录已发送的包（应用层短头 + 握手 Initial/Handshake）。
     * 纯控制帧不跟踪 — ACK/MAX_DATA/CONNECTION_CLOSE 等不需要重传。 */
    if (conn->state_ < QUIC_STATE_CLOSING && !conn->recovery_paused_) {
        int ack_eliciting = payload_ack_eliciting(payload, plen);
        if (ack_eliciting) {
            quic_recovery_on_packet_sent(&conn->recovery_ctx_,
                                        pn, 1,
                                        plen, payload, plen,
                                        pkt_type, uv_now(conn->loop_));
            /* key update / idle timer 仅对应用层（1-RTT）包 */
            if (pkt_type == -1) {
                conn->pkts_sent_with_key_++;
                maybe_initiate_key_update(conn);
                arm_idle_timer(conn);
            }
        }
    }

    return 0;
}

/* ============================================
 * ACK 管理
 * ============================================ */

static void record_rx_pn(QuicConnection *conn, int level, uint64_t pn) {
    /* 更新 largest_rx_pn_ 供后续包 PN 解码使用 */
    if (pn > conn->largest_rx_pn_[level]) {
        conn->largest_rx_pn_[level] = pn;
    }

    if (!conn->need_ack_[level]) {
        conn->ack_largest_pn_[level]  = pn;
        conn->ack_first_range_[level] = 0;
        conn->need_ack_[level]        = 1;
    } else {
        if (pn == conn->ack_largest_pn_[level] + 1) {
            conn->ack_first_range_[level]++;
            conn->ack_largest_pn_[level] = pn;
        } else if (pn > conn->ack_largest_pn_[level]) {
            /* gap detected: flush pending ACK first, then start new range */
            build_and_send_ack(conn, level);
            conn->ack_largest_pn_[level]  = pn;
            conn->ack_first_range_[level] = 0;
            conn->need_ack_[level]        = 1;
        }
        /* pn < largest: duplicate or reordered, ignored */
    }
}

static int build_and_send_ack(QuicConnection *conn, int level) {
    if (!conn->need_ack_[level]) return 0;
    conn->need_ack_[level] = 0;

    uint8_t ack_buf[256];
    QuicAckFrame ack;
    memset(&ack, 0, sizeof(ack));
    ack.largest_acknowledged = conn->ack_largest_pn_[level];
    ack.ack_delay            = 0;
    ack.num_ranges           = 0;
    ack.first_ack_range      = conn->ack_first_range_[level];

    int alen = quic_frame_write_ack(ack_buf, sizeof(ack_buf), &ack);
    if (alen < 0) {
        LOG_ERROR("[quic-conn] build_and_send_ack: ACK frame write failed, largest=%llu"
                  " first_range=%llu level=%d",
                  (unsigned long long)ack.largest_acknowledged,
                  (unsigned long long)ack.first_ack_range, level);
        return -1;
    }

    /* ACK 帧作为单独的短头包发送，或捎带在下一个出站包中 */
    /* 简化：如果 ACK 后还有数据发送，会自然捎带。这里发送单独的 ACK-only 包 */
    QuicCipherKeys *keys = &conn->keys_[level][1];
    if (!keys->initialized) {
        /* 使用 initial keys 发送 ACK */
        keys = &conn->keys_[QUIC_TLS_LEVEL_NONE][1];
    }
    if (!keys || !keys->initialized) {
        LOG_ERROR("[quic-conn] build_and_send_ack: no write keys for level=%d", level);
        return -1;
    }

    uint8_t pkt[QUIC_MAX_PKT_SIZE];
    size_t pkt_len = 0;
    uint64_t pn = conn->next_pn_[level]++;

    int pkt_type;
    if (level == QUIC_TLS_LEVEL_APPLICATION) {
        pkt_type = -1;
    } else if (level >= QUIC_TLS_LEVEL_HANDSHAKE) {
        pkt_type = QUIC_PKT_HANDSHAKE;
    } else {
        pkt_type = QUIC_PKT_INITIAL;
    }

    int ret;
    if (pkt_type >= 0) {
        ret = quic_packet_build_long(pkt, &pkt_len, pkt_type,
                                      &conn->src_cid_, &conn->dst_cid_,
                                      NULL, 0, pn, (uint8_t*)ack_buf, (size_t)alen,
                                      keys);
    } else {
        ret = quic_packet_build_short(pkt, &pkt_len, &conn->dst_cid_,
                                       pn, (uint8_t*)ack_buf, (size_t)alen, keys,
                                       conn->current_key_phase_,
                                       conn->spin_bit_);
    }
    if (ret < 0) {
        LOG_ERROR("[quic-conn] build_and_send_ack: packet build failed, pkt_type=%d"
                  " pn=%llu level=%d",
                  pkt_type, (unsigned long long)pn, level);
        return -1;
    }

    do_udp_send(conn, pkt, pkt_len);
    return 0;
}

/* ============================================
 * TLS buffer fill-bitmap helpers
 * ============================================ */

#define TLS_BM_BYTES  ((TLS_BUF_SIZE + 7) / 8)

static void tls_fill_set(uint8_t *bm, size_t off, size_t len) {
    for (size_t i = off; i < off + len; i++)
        bm[i >> 3] |= (uint8_t)(1 << (i & 7));
}

static int tls_fill_test(uint8_t *bm, size_t off) {
    return (bm[off >> 3] >> (off & 7)) & 1;
}

/* 从 contig 位置扫描 bitmap，推进到下一个未填充位置。
 * contig_/consumed_ 是绝对 CRYPTO offset，rlen_ 是 buffer 相对长度，
 * fill bitmap 随 buffer memmove 一起左移 → 相对索引 = 绝对 offset - consumed。 */
static void tls_contig_scan(QuicConnection *conn, int level) {
    uint8_t *bm = conn->tls_fill_[level];
    size_t consumed = conn->tls_consumed_[level];
    size_t c = conn->tls_contig_[level];
    size_t buf_end = consumed + conn->tls_rlen_[level];
    while (c < buf_end && tls_fill_test(bm, c - consumed))
        c++;
    conn->tls_contig_[level] = c;
}

/* ============================================
 * 入站包处理
 * ============================================ */

void QuicConnectionAccountRecvBytes(QuicConnection *conn, size_t len) {
    if (!conn) return;
    conn->bytes_recv_addr_val_ += len;
    stats_on_recv(conn, len);
}

void QuicConnectionFeedRaw(QuicConnection *conn,
                           const uint8_t *in_data, size_t in_len,
                           const char *from_ip, int from_port) {
    const uint8_t *data = in_data;
    size_t         len  = in_len;
    uint8_t       *replay_buf_to_free = NULL;  /* 非 NULL 表示来自 goto 回放 */
    if (conn->state_ == QUIC_STATE_CLOSED) return;

    arm_idle_timer(conn);

    /* RFC 9000 §8.1: 记录接收字节用于反放大检测 */
    conn->bytes_recv_addr_val_ += (uint64_t)len;
    stats_on_recv(conn, len);

    (void)from_port;
    strncpy(conn->peer_ip_, from_ip, sizeof(conn->peer_ip_) - 1);

    if (len < 1) return;

feed_replay:

    LOG_DEBUG("[quic-conn] feed %zu bytes, first_byte=0x%02x state=%d hsdone=%d",
              len, data[0], (int)conn->state_, conn->handshake_done_);

    /* Stateless Reset 检测 (RFC 9000 §10.3.1):
     * 短头包（bit7=0）+ QUIC bit 置位 + 最后 16 字节匹配已知 token → 立即关闭 */
    if (conn->reset_token_count_ > 0 &&
        !(data[0] & 0x80) && (data[0] & 0x40) && len >= 21) {
        for (int ti = 0; ti < conn->reset_token_count_; ti++) {
            if (memcmp(conn->reset_tokens_[ti],
                       data + len - 16, 16) == 0) {
                LOG_DEBUG("[quic-conn] stateless reset detected (token #%d)", ti);
                conn->state_ = QUIC_STATE_CLOSED;
                if (conn->on_close_) conn->on_close_(conn, QUIC_ERR_NO_ERROR, "");
                return;
            }
        }
    }

    while (len > 0 && conn->state_ != QUIC_STATE_CLOSED) {
    int is_long = (data[0] & 0x80) != 0;

    if (is_long) {
        int pkt_type;
        QuicConnectionId src_cid, dst_cid;
        const uint8_t *token = NULL;
        size_t token_len = 0;
        uint64_t pn = 0;
        const uint8_t *payload = NULL;
        size_t payload_len = 0;

        /* 确定保护级别以选择解密密钥 */
        int raw_type = (data[0] >> 4) & 0x03;
        /* 注意：第一字节被 HP 遮挡，这里读到的 raw_type 可能是错误的。
         * 需要先用 initial keys 尝试移除 HP。但由于 HP 只遮挡低 4 位，
         * bits 4-5 (packet type) 是 plaintext 的，所以 raw_type 应该是正确的。 */
        /* 实际上 HP 遮挡低 4 位 (bits 0-3)，bits 4-5 是 long packet type，不受 HP 影响！ */

        int level;
        QuicCipherKeys *dec_keys;
        if (raw_type == QUIC_PKT_INITIAL) {
            /* 使用 initial read keys 解密 */
            level = QUIC_TLS_LEVEL_NONE;
            dec_keys = &conn->keys_[QUIC_TLS_LEVEL_NONE][0];
            /* initial keys 可能还没放到 level 数组中，用 initial_rkeys_ */
            if (!dec_keys->initialized) {
                LOG_ERROR("[quic-conn] no initial read keys for incoming Initial pkt,"
                          " first_byte=0x%02x len=%zu", data[0], len);
                return;
            }
        } else if (raw_type == QUIC_PKT_HANDSHAKE) {
            level = QUIC_TLS_LEVEL_HANDSHAKE;
            dec_keys = &conn->keys_[QUIC_TLS_LEVEL_HANDSHAKE][0];
            if (!dec_keys->initialized) {
                /* Handshake read keys 尚未派生（代理延迟时可能先于
                 * yield_secret 到达）。缓存原始数据，等密钥就绪后
                 * yield_secret_cb 会重放这些包。 */
                if (conn->pending_hs_len_ + len > conn->pending_hs_cap_) {
                    size_t new_cap = conn->pending_hs_len_ + len + 4096;
                    if (new_cap > 65536) {
                        LOG_ERROR("[quic-conn] pending HS buffer overflow, drop");
                        return;
                    }
                    uint8_t *nb = (uint8_t*)realloc(conn->pending_hs_data_, new_cap);
                    if (!nb) return;
                    conn->pending_hs_data_ = nb;
                    conn->pending_hs_cap_  = new_cap;
                }
                memcpy(conn->pending_hs_data_ + conn->pending_hs_len_,
                       data, len);
                conn->pending_hs_len_ += len;
                LOG_DEBUG("[quic-conn] buffered %zuB Handshake pkt (keys pending, total=%zu)",
                         len, conn->pending_hs_len_);
                return;
            }
        } else {
            LOG_WARN("[quic-conn] unsupported long header pkt type=%d, first_byte=0x%02x len=%zu",
                     raw_type, data[0], len);
            return; /* 不支持的包类型 */
        }

        size_t consumed = 0;
        int ret = quic_packet_parse_long(data, len, &pkt_type,
                                          &src_cid, &dst_cid,
                                          &token, &token_len,
                                          &pn, &payload, &payload_len,
                                          dec_keys,
                                          conn->largest_rx_pn_[level],
                                          &consumed);
        if (ret < 0) {
            LOG_ERROR("[quic-conn] failed to parse long header packet, pkt_type=%d"
                      " first_byte=0x%02x len=%zu expected_pn=%llu",
                      raw_type, data[0], len,
                      (unsigned long long)conn->largest_rx_pn_[level]);
            return;
        }

        /* 收到客户端有效 Handshake 包 → 地址验证完成，解除 3x 反放大限制 */
        if (raw_type == QUIC_PKT_HANDSHAKE && !conn->peer_address_validated_) {
            conn->peer_address_validated_ = 1;
            LOG_DEBUG("[quic-conn] peer address validated (recv Handshake pkt), anti-amplification off");
        }

        if (!conn->handshake_done_) {
            LOG_DEBUG("[quic-conn] recv %s pkt PN=%llu payload=%zuB",
                     raw_type == QUIC_PKT_INITIAL ? "Initial" :
                     raw_type == QUIC_PKT_HANDSHAKE ? "Handshake" : "1-RTT",
                     (unsigned long long)pn, payload_len);
        }

        record_rx_pn(conn, level, pn);

        /* 解析帧 */
        size_t pos = 0;
        while (pos < payload_len) {
            uint8_t ftype = payload[pos];
            size_t fpos = pos;
            int consumed = 0;

            switch (ftype) {
            case QUIC_FRAME_PADDING:
                /* pos incremented below (consumed==0 → pos+=1) */
                break;
            case QUIC_FRAME_PING:
                /* pos incremented below (consumed==0 → pos+=1) */
                break;
            case QUIC_FRAME_CRYPTO: {
                uint64_t coff;
                const uint8_t *cdata;
                size_t clen;
                consumed = quic_frame_parse_crypto(payload + pos,
                                                    payload_len - pos,
                                                    &coff, &cdata, &clen);
                if (consumed < 0) {
                    LOG_ERROR("[quic-conn] CRYPTO frame parse failed in long header,"
                              " pos=%zu payload_len=%zu", pos, payload_len);
                    goto next_pkt;
                }

                int rlev = level;
                /* 期望的下一个偏移 = 已消费 + 缓冲区中待处理 */
                size_t expected_off = conn->tls_consumed_[rlev]
                                    + conn->tls_rlen_[rlev];
                if (coff == expected_off) {
                    if (conn->tls_rlen_[rlev] + clen <= conn->tls_rcap_[rlev]) {
                        memcpy(conn->tls_rbuf_[rlev] + conn->tls_rlen_[rlev],
                               cdata, clen);
                        tls_fill_set(conn->tls_fill_[rlev],
                                     conn->tls_rlen_[rlev], clen);
                        conn->tls_rlen_[rlev] += clen;
                    }
                    if ((size_t)coff == conn->tls_contig_[rlev]) {
                        conn->tls_contig_[rlev] += clen;
                        tls_contig_scan(conn, rlev);
                    }
                    LOG_DEBUG("[quic-conn] fed %zu bytes CRYPTO at level=%d off=%llu total=%zu"
                             " contig=%zu expected=%zu",
                           clen, rlev, (unsigned long long)coff,
                           conn->tls_rlen_[rlev], conn->tls_contig_[rlev],
                           expected_off);
                } else if (coff < expected_off) {
                    LOG_DEBUG("[quic-conn] CRYPTO retrans L%d: coff=%llu exp=%zu"
                             " contig=%zu rlen=%zu",
                             rlev, (unsigned long long)coff, expected_off,
                             conn->tls_contig_[rlev], conn->tls_rlen_[rlev]);
                    /* 重传填充 gap — coff 是绝对 CRYPTO offset。
                     * tls_rbuf_ 已被 crypto_release 移位 consumed 字节，
                     * 写入时需转为相对偏移。 */
                    if (conn->tls_rcap_[rlev] >= coff + clen) {
                        size_t abs_off = (size_t)coff;       /* 绝对 */
                        size_t abs_end = abs_off + clen;
                        size_t consumed = conn->tls_consumed_[rlev];
                        if (abs_end <= consumed) goto skip_retrans;
                        /* 截去已消费前缀 */
                        if (abs_off < consumed) abs_off = consumed;
                        size_t rel_off = abs_off - consumed; /* 相对 */
                        size_t wrt_len = abs_end - abs_off;
                        memcpy(conn->tls_rbuf_[rlev] + rel_off,
                               cdata + (abs_off - (size_t)coff), wrt_len);
                        tls_fill_set(conn->tls_fill_[rlev], rel_off, wrt_len);
                        if (rel_off + wrt_len > conn->tls_rlen_[rlev])
                            conn->tls_rlen_[rlev] = rel_off + wrt_len;
                        if (abs_off == conn->tls_contig_[rlev]) {
                            conn->tls_contig_[rlev] += wrt_len;
                            tls_contig_scan(conn, rlev);
                        }
                    }
skip_retrans:
                    LOG_DEBUG("[quic-conn] retrans CRYPTO L%d off=%llu clen=%zu"
                             " contig=%zu rlen=%zu consumed=%zu",
                           rlev, (unsigned long long)coff, clen,
                           conn->tls_contig_[rlev], conn->tls_rlen_[rlev],
                           conn->tls_consumed_[rlev]);
                } else {
                    LOG_DEBUG("[quic-conn] CRYPTO gap L%d: coff=%llu exp=%zu"
                             " gap=%zu",
                             rlev, (unsigned long long)coff, expected_off,
                             (size_t)(coff - expected_off));
                    /* gap: 零填充 + 存数据，contig_ 不推进 */
                    size_t gap = (size_t)(coff - expected_off);
                    size_t need = conn->tls_rlen_[rlev] + gap + clen;
                    if (need > conn->tls_rcap_[rlev]) {
                        /* 扩容（R2：首包统一走 FeedRaw 后，大 offset gap
                         * 需扩容，否则首包 CRYPTO 会被静默丢弃） */
                        size_t new_cap = need * 2;
                        uint8_t *nb = (uint8_t*)realloc(conn->tls_rbuf_[rlev], new_cap);
                        if (!nb) goto next_pkt;
                        conn->tls_rbuf_[rlev] = nb;
                        conn->tls_rcap_[rlev] = new_cap;
                    }
                    if (need <= conn->tls_rcap_[rlev]) {
                        /* gap 区域不设 bitmap — 零填充标记 */
                        memset(conn->tls_rbuf_[rlev] + conn->tls_rlen_[rlev],
                               0, gap);
                        conn->tls_rlen_[rlev] += gap;
                        memcpy(conn->tls_rbuf_[rlev] + conn->tls_rlen_[rlev],
                               cdata, clen);
                        tls_fill_set(conn->tls_fill_[rlev],
                                     conn->tls_rlen_[rlev], clen);
                        conn->tls_rlen_[rlev] += clen;
                    }
                    LOG_DEBUG("[quic-conn] CRYPTO gap L%d: off=%llu expected=%zu"
                             " rlen=%zu contig=%zu",
                           rlev, (unsigned long long)coff, expected_off,
                           conn->tls_rlen_[rlev], conn->tls_contig_[rlev]);
                }

                /* 任何 CRYPTO 帧处理都可能推进 contig（retrans 或 gap 填充
                 * 后 bitmap 标记了数据），无条件扫描确保不遗漏。 */
                if (!conn->handshake_done_)
                    tls_contig_scan(conn, rlev);

                /* 驱动握手 — 如是延迟模式则跳过 */
                if (!conn->handshake_done_ && !conn->handshake_deferred_) {
                    int hr = SSL_do_handshake(conn->ssl_);
                    if (hr == 1) {
                        conn->handshake_done_ = 1;
                        conn->state_ = QUIC_STATE_ESTABLISHED;
                        LOG_DEBUG("[quic-conn] HS-DONE (client)"
                                 " L0-contig=%zu L0-rlen=%zu L0-cons=%zu"
                                 " L2-contig=%zu L2-rlen=%zu L2-cons=%zu"
                                 " pending=%zu",
                                 conn->tls_contig_[0], conn->tls_rlen_[0], conn->tls_consumed_[0],
                                 conn->tls_contig_[2], conn->tls_rlen_[2], conn->tls_consumed_[2],
                                 conn->pending_hs_len_);

                        /* 握手完成 → 清理 Initial + Handshake PN 空间 */
                        quic_recovery_handshake_done(&conn->recovery_ctx_, 0);

                        /* 读取 TLS 协商的 cipher suite */
                        quic_crypto_set_cipher_suite_from_ssl(conn->ssl_);

                        /* 服务端发送 HANDSHAKE_DONE 帧 */
                        if (conn->stream_ctx_.next_local_bidi_id == 1) {
                            static const uint8_t hd_frame[1] = { QUIC_FRAME_HANDSHAKE_DONE };
                            quic_conn_queue_frames(conn, hd_frame, 1);
                        }

                        /* 初始化流控 */
                        quic_stream_handshake_done(&conn->stream_ctx_, conn,
                                                    quic_conn_queue_frames);

                        if (conn->on_connected_) conn->on_connected_(conn);

                        /* flush 握手完成后的数据包（MAX_DATA + 应用数据） */
                        quic_conn_flush_packet(conn);
                    } else {
                        int err = SSL_get_error(conn->ssl_, hr);
                        LOG_DEBUG("[quic-conn] SSL_do_handshake not done: ret=%d err=%d"
                                 " (WANT_READ=%d) L0-contig=%zu/rlen=%zu/cons=%zu"
                                 " L2-contig=%zu/rlen=%zu/cons=%zu"
                                 " pending_hs=%zu hsdone=%d",
                                 hr, err, SSL_ERROR_WANT_READ,
                                 conn->tls_contig_[0], conn->tls_rlen_[0], conn->tls_consumed_[0],
                                 conn->tls_contig_[2], conn->tls_rlen_[2], conn->tls_consumed_[2],
                                 conn->pending_hs_len_, conn->handshake_done_);
                    }
                    /* 握手未完成时，握手包已登记进 recovery（R3），
                     * PTO 会精确重传丢失的 CRYPTO 分片，无需额外定时器。 */
                }
                /* 确保 handshake_deferred 在下一帧被清除 —
                 * yield_secret_cb 设置此标记防止重入，但客户端没有
                 * CryptoFlushHandshake 来清除它。 */
                conn->handshake_deferred_ = 0;
                break;
            }
            case QUIC_FRAME_ACK:
            case QUIC_FRAME_ACK_ECN:
                /* 握手中的 ACK（Initial/Handshake PN 空间）→ 送入恢复层 */
                {
                    QuicAckFrame af;
                    consumed = quic_frame_parse_ack(payload + pos,
                                                     payload_len - pos, &af);
                    if (consumed < 0) {
                        LOG_ERROR("[quic-conn] ACK frame parse failed in long header,"
                                  " pos=%zu payload_len=%zu", pos, payload_len);
                        goto next_pkt;
                    }
                    quic_recovery_on_ack_received(&conn->recovery_ctx_, &af,
                                                   uv_now(conn->loop_), level);
                    LOG_DEBUG("[quic-conn] ACK: largest=%llu",
                           (unsigned long long)af.largest_acknowledged);
                }
                break;
            case QUIC_FRAME_DATA_BLOCKED: {
                uint64_t md;
                consumed = quic_frame_parse_data_blocked(payload + pos,
                                                         payload_len - pos, &md);
                if (consumed < 0) goto next_pkt;
                quic_stream_on_data_blocked(&conn->stream_ctx_, md);
                break;
            }
            case QUIC_FRAME_STREAM_DATA_BLOCKED: {
                uint64_t sid, msd;
                consumed = quic_frame_parse_stream_data_blocked(payload + pos,
                                                                payload_len - pos,
                                                                &sid, &msd);
                if (consumed < 0) goto next_pkt;
                quic_stream_on_stream_data_blocked(&conn->stream_ctx_, sid, msd);
                break;
            }
            case QUIC_FRAME_STREAMS_BLOCKED_BIDI:
            case QUIC_FRAME_STREAMS_BLOCKED_UNI: {
                uint64_t ms;
                int bidi;
                consumed = quic_frame_parse_streams_blocked(payload + pos,
                                                            payload_len - pos,
                                                            &ms, &bidi);
                if (consumed < 0) goto next_pkt;
                quic_stream_on_streams_blocked(&conn->stream_ctx_, ms, bidi);
                break;
            }
            case QUIC_FRAME_RETIRE_CONNECTION_ID: {
                uint64_t rseq;
                consumed = quic_frame_parse_retire_conn_id(payload + pos,
                                                           payload_len - pos, &rseq);
                if (consumed < 0) goto next_pkt;
                LOG_DEBUG("[quic-conn] peer retired CID seq=%llu", (unsigned long long)rseq);
                break;
            }
            case QUIC_FRAME_CONNECTION_CLOSE:
            case QUIC_FRAME_CONNECTION_CLOSE_APP: {
                uint64_t ec;
                const char *reason;
                size_t rlen;
                consumed = quic_frame_parse_connection_close(
                    payload + pos, payload_len - pos, &ec, &reason, &rlen);
                if (consumed < 0) {
                    LOG_ERROR("[quic-conn] CONNECTION_CLOSE frame parse failed in long header,"
                              " pos=%zu payload_len=%zu", pos, payload_len);
                    goto next_pkt;
                }
                LOG_INFO("[quic-conn] peer CONNECTION_CLOSE: err=0x%llx reason=%.*s",
                       (unsigned long long)ec, (int)rlen, reason);
                /* 延迟关闭：让后续包（如 echo STREAM）有机会被处理 */
                if (conn->state_ < QUIC_STATE_CLOSING) {
                    conn->state_ = QUIC_STATE_CLOSING;
                    conn->close_ec_ = ec;
                    snprintf(conn->close_reason_, sizeof(conn->close_reason_),
                             "%.*s", (int)rlen, reason);
                    /* 1 秒后正式关闭 */
                    quic_timer_start(&conn->close_timer_, on_close_timer, conn, 1000, 0);
                }
                goto next_pkt;
            }
            default:
                /* 未知帧类型，跳过 1 字节继续 */
                LOG_WARN("[quic-conn] unknown frame type 0x%02x in long header,"
                         " pos=%zu payload_len=%zu", ftype, pos, payload_len);
                consumed = 1;
                break;
            }
            pos += (consumed > 0) ? (size_t)consumed : 1;
        }

next_pkt:

        /* 发送 ACK（如果有未确认的包） */
        build_and_send_ack(conn, level);

        /* 前进到下一个合并包 */
        data += consumed;
        len -= consumed;
    } else {
        /* --------------------------------------------------
         * 短头包 — 支持 Key Phase 检测 (RFC 9001 §6)
         *
         * Key Phase 位被 Header Protection 保护。正确的方法是：
         * 1. HP 移除后检查 reserved bits (bits 4-3 必须为 0)
         * 2. 如果 reserved bits != 0 → HP 密钥错误 → 尝试下一代密钥
         * 3. 如果 reserved bits == 0 → 密钥正确，继续 AEAD 解密
         * -------------------------------------------------- */
        if (!conn->keys_[QUIC_TLS_LEVEL_APPLICATION][0].initialized) {
            /* app read key 未就绪（握手未完成）→ 缓冲，等 yield_secret
             * 派生后回放。否则对端早发的 1-RTT 包（如 H3 SETTINGS）被
             * 丢弃 → SETTINGS 交换死锁。 */
            if (conn->pending_1rtt_len_ + len > conn->pending_1rtt_cap_) {
                size_t new_cap = conn->pending_1rtt_len_ + len + 4096;
                if (new_cap > 65536) {
                    LOG_ERROR("[quic-conn] pending 1-RTT buffer overflow, drop");
                    return;
                }
                uint8_t *nb = (uint8_t*)realloc(conn->pending_1rtt_data_, new_cap);
                if (!nb) return;
                conn->pending_1rtt_data_ = nb;
                conn->pending_1rtt_cap_  = new_cap;
            }
            memcpy(conn->pending_1rtt_data_ + conn->pending_1rtt_len_, data, len);
            conn->pending_1rtt_len_ += len;
            LOG_DEBUG("[quic-conn] buffered %zuB 1-RTT pkt (app keys pending, total=%zu)",
                     len, conn->pending_1rtt_len_);
            return;
        }

        uint8_t dcid_len = conn->src_cid_.len;
        if (dcid_len == 0) dcid_len = QUIC_CID_LEN;
        if (len < 1 + dcid_len + 1) {
            LOG_ERROR("[quic-conn] FeedRaw: short header too small, len=%zu dcid_len=%u",
                      len, dcid_len);
            return;
        }

        size_t pn_offset = 1 + dcid_len;
        /* Minimum: 1-byte PN + 16-byte AEAD tag. HP removal has its
         * own stricter check (sample at pn_offset+4 needs 16 bytes). */
        if (pn_offset + 1 + 16 > len) {
            LOG_ERROR("[quic-conn] FeedRaw: short header not enough data, pn_offset=%zu len=%zu",
                      pn_offset, len);
            return;
        }

        uint8_t pkt_buf[QUIC_UDP_BUF_SIZE];
        uint8_t pkt_orig[QUIC_UDP_BUF_SIZE];
        if (len > sizeof(pkt_buf)) {
            LOG_ERROR("[quic-conn] FeedRaw: short header len=%zu exceeds buffer=%zu",
                      len, sizeof(pkt_buf));
            return;
        }
        memcpy(pkt_orig, data, len);

        /* 尝试 HP 移除：先当前密钥，reserved bits 不对则试下一代 */
        int    keys_ok     = 0;
        size_t pn_len      = 0;
        uint64_t pn        = 0;
        int    key_phase   = 0;
        size_t ct_pos      = 0;   /* 密文起始偏移 */
        size_t pt_len      = 0;   /* 解密后明文长度 */

        for (int try = 0; try < 2; try++) {
            memcpy(pkt_buf, pkt_orig, len);

            int pn_len_i = quic_crypto_hp_remove_short(
                &conn->keys_[QUIC_TLS_LEVEL_APPLICATION][0],
                pkt_buf, len, pn_offset);
            if (pn_len_i <= 0) {
                LOG_DEBUG("[quic-conn] short hdr try=%d: HP remove failed", try);
                goto try_next_keys;
            }
            pn_len = (size_t)pn_len_i;

            /* RFC 9000 §17.3: reserved bits (4-3) 必须为 0 */
            int reserved = (pkt_buf[0] >> 3) & 0x03;
            if (reserved != 0) {
                LOG_DEBUG("[quic-conn] short hdr try=%d: reserved=%d (0x%02x), wrong key",
                          try, reserved, pkt_buf[0]);
                goto try_next_keys;
            }

            key_phase = (pkt_buf[0] >> 2) & 0x01;
            conn->spin_bit_ = (pkt_buf[0] >> 5) & 0x01;  /* 回显对端 spin bit */
            pn = quic_pn_decode(pkt_buf + pn_offset, pn_len,
                                conn->largest_rx_pn_[QUIC_TLS_LEVEL_APPLICATION]);

            /* 校验 DCID */
            {
                QuicConnectionId dcid;
                memcpy(dcid.data, pkt_buf + 1, dcid_len);
                dcid.len = dcid_len;
                if (!quic_cid_eq(&dcid, &conn->src_cid_)) {
                    LOG_DEBUG("[quic-conn] short header DCID mismatch");
                    return;
                }
            }

            /* AEAD 解密 */
            ct_pos = pn_offset + pn_len;
            if (ct_pos + 16 > len) {
                LOG_ERROR("[quic-conn] FeedRaw: ct too short, ct_pos=%zu len=%zu",
                          ct_pos, len);
                return;
            }

            /* DEBUG: dump AAD */
            {
                char hex[256];
                size_t hpos = 0;
                size_t aad_len = pn_offset + pn_len;
                for (size_t i = 0; i < aad_len && hpos < sizeof(hex) - 3; i++)
                    hpos += snprintf(hex + hpos, sizeof(hex) - hpos, "%02x", pkt_buf[i]);
                LOG_DEBUG("[quic-conn] short decrypt try=%d: aad[%zu]=%s pn_offset=%zu pn_len=%zu dcid_len=%u",
                         try, aad_len, hex, pn_offset, pn_len, dcid_len);
            }

            int ret = quic_crypto_decrypt(
                &conn->keys_[QUIC_TLS_LEVEL_APPLICATION][0], pn,
                pkt_buf, pn_offset + pn_len,
                pkt_buf + ct_pos, len - ct_pos,
                pkt_buf + ct_pos, &pt_len);

            if (ret == 0) {
                keys_ok = 1;
                LOG_DEBUG("[quic-conn] short hdr try=%d: OK pn=%llu phase=%d pt=%zu",
                          try, (unsigned long long)pn, key_phase, pt_len);
                break;
            }

            LOG_DEBUG("[quic-conn] short hdr try=%d: AEAD failed pn=%llu"
                     " pn_len=%zu pn_offset=%zu len=%zu",
                     try, (unsigned long long)pn, pn_len, pn_offset, len);

            /* Try alternate PN for 1-byte-encoded packets that arrive
             * after largest_rx_pn has advanced past 256.
             * quic_pn_decode wraps the raw wire byte (true PN 0-255)
             * forward by N×256 to match largest_rx_pn.
             * Try the raw wire value as PN first, then +256, -256. */
            if (try == 0 && pn_len == 1 && pn >= 256) {
                LOG_DEBUG("[quic-conn] alt PN: wire=%u decoded=%llu",
                         (unsigned)pkt_buf[pn_offset], (unsigned long long)pn);
                uint64_t wire_val = (uint64_t)pkt_buf[pn_offset];
                /* Candidates: raw value (if old), +256 (if same window offset),
                 * and powers-of-256 offsets to cover deep reordering */
                uint64_t candidates[6];
                int nc = 0;
                candidates[nc++] = wire_val;           /* raw 0-255 */
                candidates[nc++] = wire_val + 256;     /* one window forward */
                if (pn >= 512)
                    candidates[nc++] = pn - 512;
                if (pn >= 768)
                    candidates[nc++] = pn - 768;
                /* dedup and try each */
                for (int ci = 0; ci < nc; ci++) {
                    if (candidates[ci] == pn) continue;
                    /* dedup against earlier candidates */
                    int dup = 0;
                    for (int cj = 0; cj < ci; cj++)
                        if (candidates[cj] == candidates[ci]) { dup = 1; break; }
                    if (dup) continue;

                    memcpy(pkt_buf + ct_pos, pkt_orig + ct_pos, len - ct_pos);
                    int alt_ret = quic_crypto_decrypt(
                        &conn->keys_[QUIC_TLS_LEVEL_APPLICATION][0],
                        candidates[ci],
                        pkt_buf, pn_offset + pn_len,
                        pkt_buf + ct_pos, len - ct_pos,
                        pkt_buf + ct_pos, &pt_len);
                    if (alt_ret == 0) {
                        pn = candidates[ci];
                        keys_ok = 1;
                        LOG_DEBUG("[quic-conn] short hdr try=%d: OK (alt PN)"
                                  " pn=%llu wire=%llu decoded=%llu",
                                  try, (unsigned long long)pn,
                                  (unsigned long long)wire_val,
                                  (unsigned long long)
                                  (pn_len==1 ? (uint64_t)pkt_buf[pn_offset] : 0));
                        break;
                    }
                }
                if (!keys_ok) {
                    LOG_DEBUG("[quic-conn] short hdr try=%d: alt PN all failed"
                             " wire=%llu decoded=%llu",
                             try, (unsigned long long)wire_val,
                             (unsigned long long)pn);
                }
                /* Restore CT for the normal try_next_keys path */
                if (!keys_ok)
                    memcpy(pkt_buf + ct_pos, pkt_orig + ct_pos, len - ct_pos);
                else
                    break;
            }

try_next_keys:
            if (try == 0 && conn->app_read_secret_saved_) {
                /* 保存旧密钥 */
                QuicCipherKeys old_read  = conn->keys_[QUIC_TLS_LEVEL_APPLICATION][0];
                QuicCipherKeys old_write = conn->keys_[QUIC_TLS_LEVEL_APPLICATION][1];
                uint8_t old_read_sec[32], old_write_sec[32];
                memcpy(old_read_sec,  conn->app_read_secret_,  32);
                if (conn->app_write_secret_saved_)
                    memcpy(old_write_sec, conn->app_write_secret_, 32);
                int old_phase = conn->peer_key_phase_;

                /* 推导下一代 AEAD (key+iv)，但保留 HP key 不变。
                 * quic-go 的 rollKeys() 不轮转 header protector。 */
                QuicCipherKeys next_read  = old_read;  /* 复用 hp_key */
                uint8_t next_rs[32];
                int ok = (quic_crypto_derive_key_update_aead(&next_read,
                           conn->app_read_secret_, 32, next_rs, sizeof(next_rs)) == 0);

                QuicCipherKeys next_write = old_write;
                uint8_t next_ws[32];
                int wk_ok = 0;
                if (conn->app_write_secret_saved_) {
                    wk_ok = (quic_crypto_derive_key_update_aead(&next_write,
                                conn->app_write_secret_, 32, next_ws, sizeof(next_ws)) == 0);
                }

                if (!ok) {
                    LOG_ERROR("[quic-conn] key update aead derivation failed");
                    return;
                }

                conn->keys_[QUIC_TLS_LEVEL_APPLICATION][0] = next_read;
                memcpy(conn->app_read_secret_, next_rs, 32);
                if (wk_ok) {
                    conn->keys_[QUIC_TLS_LEVEL_APPLICATION][1] = next_write;
                    memcpy(conn->app_write_secret_, next_ws, 32);
                }
                conn->peer_key_phase_   = conn->peer_key_phase_ ^ 1;
                conn->current_key_phase_ = conn->peer_key_phase_;
                conn->pkts_sent_with_key_ = 0;
                conn->aead_pkt_count_[QUIC_TLS_LEVEL_APPLICATION] = 0;
                conn->aead_limit_warned_[QUIC_TLS_LEVEL_APPLICATION] = 0;
                LOG_DEBUG("[quic-conn] key update: trying gen=%d (hp unchanged)",
                         conn->peer_key_phase_);

                /* retry: HP key 未变，PN/offset 复用 try 0 的结果。
                 * 但 try 0 的失败 AEAD 解密破坏了 pkt_buf 中的密文，
                 * 需要从 pkt_orig 恢复。 */
                {
                    memcpy(pkt_buf + ct_pos, pkt_orig + ct_pos, len - ct_pos);
                    key_phase = (pkt_buf[0] >> 2) & 0x01;
                    conn->spin_bit_ = (pkt_buf[0] >> 5) & 0x01;

                    /* DEBUG: dump retry AAD */
                    {
                        char hex[256];
                        size_t hpos = 0;
                        size_t al = pn_offset + pn_len;
                        for (size_t i = 0; i < al && hpos < sizeof(hex) - 3; i++)
                            hpos += snprintf(hex + hpos, sizeof(hex) - hpos, "%02x", pkt_buf[i]);
                        LOG_DEBUG("[quic-conn] short decrypt retry: aad[%zu]=%s pn=%llu pn_offset=%zu pn_len=%zu",
                                 al, hex, (unsigned long long)pn, pn_offset, pn_len);
                    }

                    int ret2 = quic_crypto_decrypt(
                        &conn->keys_[QUIC_TLS_LEVEL_APPLICATION][0], pn,
                        pkt_buf, pn_offset + pn_len,
                        pkt_buf + ct_pos, len - ct_pos,
                        pkt_buf + ct_pos, &pt_len);

                    if (ret2 == 0) {
                        keys_ok = 1;
                        LOG_DEBUG("[quic-conn] key update: gen=%d confirmed, pn=%llu",
                                 conn->peer_key_phase_, (unsigned long long)pn);
                        break;
                    }

                    LOG_DEBUG("[quic-conn] key update: gen=%d AEAD failed, rolling back",
                              conn->peer_key_phase_);
                }

rollback:
                /* 回滚密钥 */
                conn->keys_[QUIC_TLS_LEVEL_APPLICATION][0] = old_read;
                conn->keys_[QUIC_TLS_LEVEL_APPLICATION][1] = old_write;
                memcpy(conn->app_read_secret_,  old_read_sec,  32);
                if (conn->app_write_secret_saved_)
                    memcpy(conn->app_write_secret_, old_write_sec, 32);
                conn->peer_key_phase_   = old_phase;
                conn->current_key_phase_ = old_phase;
                LOG_DEBUG("[quic-conn] key update: rolled back to gen=%d", old_phase);
            }
            /* 两轮都失败 */
            LOG_ERROR("[quic-conn] short header decrypt failed after key retry:"
                      " len=%zu dcid_len=%u pn_offset=%zu",
                      len, dcid_len, pn_offset);
            return;
        }

        if (!keys_ok) return;

        /* 收到首个 1-RTT 包 → 对端已完成握手（服务端收到客户端 Finished，
         * 客户端收到服务端 Finished），可安全清理 Handshake 空间。 */
        if (!conn->handshake_space_cleared_) {
            conn->handshake_space_cleared_ = 1;
            quic_recovery_clear_handshake(&conn->recovery_ctx_);
        }

        /* Key Phase 同步 — 只更新 AEAD key+iv，保留 HP key */
        if (key_phase != conn->peer_key_phase_) {
            if (conn->current_key_phase_ == conn->peer_key_phase_) {
                if (conn->app_write_secret_saved_) {
                    QuicCipherKeys next_wk = conn->keys_[QUIC_TLS_LEVEL_APPLICATION][1];
                    uint8_t next_ws[32];
                    if (quic_crypto_derive_key_update_aead(&next_wk,
                                                       conn->app_write_secret_, 32,
                                                       next_ws, sizeof(next_ws)) == 0) {
                        conn->keys_[QUIC_TLS_LEVEL_APPLICATION][1] = next_wk;
                        memcpy(conn->app_write_secret_, next_ws, 32);
                    }
                }
            }
            conn->peer_key_phase_   = key_phase;
            conn->current_key_phase_ = key_phase;
            conn->pkts_sent_with_key_ = 0;  /* 新 phase 重新计数 */
            conn->aead_pkt_count_[QUIC_TLS_LEVEL_APPLICATION] = 0;
            conn->aead_limit_warned_[QUIC_TLS_LEVEL_APPLICATION] = 0;
            LOG_DEBUG("[quic-conn] key phase synced: %d", key_phase);
        }

        /* pkt_buf 在 ct_pos 处已经是解密后的明文 (try 循环中解密成功) */
        {
            const uint8_t *payload = pkt_buf + ct_pos;
            size_t payload_len = pt_len;

            /* DEBUG: dump first AND last 64 bytes of decrypted payload */
            {
                char hex[384];
                size_t hpos = 0;
                size_t dump_n = payload_len < 64 ? payload_len : 64;
                for (size_t i = 0; i < dump_n && hpos < sizeof(hex) - 3; i++)
                    hpos += snprintf(hex + hpos, sizeof(hex) - hpos,
                                     "%02x", payload[i]);
                LOG_DEBUG("[quic-conn] short decrypted head[%zu]: %s pn=%llu ct_pos=%zu pt_len=%zu",
                         dump_n, hex, (unsigned long long)pn, ct_pos, pt_len);
                /* dump tail */
                if (payload_len > 128) {
                    hpos = 0;
                    size_t tail_start = payload_len - 64;
                    for (size_t i = tail_start; i < payload_len && hpos < sizeof(hex) - 3; i++)
                        hpos += snprintf(hex + hpos, sizeof(hex) - hpos,
                                         "%02x", payload[i]);
                    LOG_DEBUG("[quic-conn] short decrypted tail[%zu..%zu]: %s",
                             tail_start, payload_len, hex);
                }
            }

            record_rx_pn(conn, QUIC_TLS_LEVEL_APPLICATION, pn);

            /* ---- 解析帧 ---- */
            size_t pos = 0;
            while (pos < payload_len) {
                uint8_t ftype = payload[pos];
                int consumed = 0;

                switch (ftype) {
                case QUIC_FRAME_PADDING:         consumed = 1; break;
                case QUIC_FRAME_PING:            consumed = 1; break;
                case QUIC_FRAME_HANDSHAKE_DONE:  consumed = 1; break;
                case QUIC_FRAME_DATAGRAM:
                case QUIC_FRAME_DATAGRAM_LEN: {
                    const uint8_t *ddata;
                    size_t dlen;
                    consumed = quic_frame_parse_datagram(payload + pos,
                                                         payload_len - pos,
                                                         &ddata, &dlen);
                    if (consumed > 0 && conn->on_datagram_)
                        conn->on_datagram_(conn, ddata, dlen);
                    else if (consumed <= 0)
                        consumed = 1; /* skip unknown byte */
                    break;
                }
                case QUIC_FRAME_CRYPTO: {
                    uint64_t coff;
                    const uint8_t *cdata;
                    size_t clen;
                    consumed = quic_frame_parse_crypto(payload + pos,
                                                        payload_len - pos,
                                                        &coff, &cdata, &clen);
                    if (consumed < 0) {
                        LOG_ERROR("[quic-conn] CRYPTO frame parse failed in short header,"
                                  " pos=%zu payload_len=%zu", pos, payload_len);
                        goto done_short;
                    }

                    int rlev = QUIC_TLS_LEVEL_APPLICATION;
                    size_t expected_off = conn->tls_consumed_[rlev]
                                        + conn->tls_rlen_[rlev];
                    if (coff == expected_off) {
                        if (conn->tls_rlen_[rlev] + clen <= conn->tls_rcap_[rlev]) {
                            memcpy(conn->tls_rbuf_[rlev] + conn->tls_rlen_[rlev],
                                   cdata, clen);
                            conn->tls_rlen_[rlev] += clen;
                        }
                        LOG_DEBUG("[quic-conn] fed %zu bytes CRYPTO at level=%d, total=%zu",
                               clen, rlev, conn->tls_rlen_[rlev]);
                    } else {
                        LOG_DEBUG("[quic-conn] skip post-handshake CRYPTO off=%llu expected=%zu",
                               (unsigned long long)coff, expected_off);
                    }

                    if (!conn->handshake_deferred_)
                        SSL_do_handshake(conn->ssl_);
                    break;
                }
                case QUIC_FRAME_CONNECTION_CLOSE:
                case QUIC_FRAME_CONNECTION_CLOSE_APP: {
                    uint64_t ec;
                    const char *reason;
                    size_t rlen;
                    consumed = quic_frame_parse_connection_close(
                        payload + pos, payload_len - pos, &ec, &reason, &rlen);
                    if (consumed < 0) {
                        LOG_ERROR("[quic-conn] CONNECTION_CLOSE frame parse failed in short header,"
                                  " pos=%zu payload_len=%zu", pos, payload_len);
                        goto done_short;
                    }
                    LOG_INFO("[quic-conn] peer CONNECTION_CLOSE: err=0x%llx reason=%.*s",
                           (unsigned long long)ec,
                           (int)rlen, reason ? reason : "");
                    /* 延迟关闭：让后续包（如 echo STREAM）有机会被处理 */
                    if (conn->state_ < QUIC_STATE_CLOSING) {
                        conn->state_ = QUIC_STATE_CLOSING;
                        conn->close_ec_ = ec;
                        quic_timer_start(&conn->close_timer_, on_close_timer, conn, 1000, 0);
                    }
                    goto done_short;
                }
                case QUIC_FRAME_ACK:
                case QUIC_FRAME_ACK_ECN: {
                    QuicAckFrame af;
                    consumed = quic_frame_parse_ack(payload + pos,
                                                     payload_len - pos, &af);
                    if (consumed < 0) {
                        LOG_ERROR("[quic-conn] ACK frame parse failed in short header,"
                                  " type=0x%02x pos=%zu payload_len=%zu",
                                  ftype, pos, payload_len);
                        goto done_short;
                    }
                    quic_recovery_on_ack_received(&conn->recovery_ctx_, &af,
                                                   uv_now(conn->loop_),
                                                   QUIC_TLS_LEVEL_APPLICATION);
                    /* ACK may have freed CWND — retry blocked streams */
                    quic_stream_flush_pending(&conn->stream_ctx_, conn,
                                               quic_conn_queue_frames,
                                               quic_conn_flush_packet);
                    /* PMTU probe tracking: if probe was ACKed, upgrade */
                    if (conn->probe_size_ > 0 &&
                        af.largest_acknowledged >= conn->probe_pn_) {
                        conn->current_pmtu_ = conn->probe_size_;
                        conn->probe_size_ = 0;
                        LOG_DEBUG("[quic-conn] PMTU upgraded to %llu",
                                 (unsigned long long)conn->current_pmtu_);
                    }
                    break;
                }
                case QUIC_FRAME_STREAM:
                case QUIC_FRAME_STREAM + 1:
                case QUIC_FRAME_STREAM + 2:
                case QUIC_FRAME_STREAM + 3:
                case QUIC_FRAME_STREAM + 4:
                case QUIC_FRAME_STREAM + 5:
                case QUIC_FRAME_STREAM + 6:
                case QUIC_FRAME_STREAM + 7: {
                    uint64_t sid, off;
                    int fin;
                    const uint8_t *sd;
                    size_t slen;
                    consumed = quic_frame_parse_stream(payload + pos,
                                                        payload_len - pos,
                                                        &sid, &off, &fin,
                                                        &sd, &slen);
                    if (consumed < 0) break;
                    quic_stream_on_recv(&conn->stream_ctx_, conn,
                                         sid, off, fin, sd, slen,
                                         quic_conn_queue_frames,
                                         quic_conn_flush_packet);
                    break;
                }
                case QUIC_FRAME_MAX_DATA: {
                    uint64_t md;
                    consumed = quic_frame_parse_max_data(payload + pos,
                                                          payload_len - pos, &md);
                    if (consumed < 0) break;
                    quic_stream_on_max_data(&conn->stream_ctx_, md);
                    quic_conn_retry_send(conn);
                    break;
                }
                case QUIC_FRAME_MAX_STREAM_DATA: {
                    uint64_t sid, md;
                    consumed = quic_frame_parse_max_stream_data(payload + pos,
                                                                 payload_len - pos,
                                                                 &sid, &md);
                    if (consumed < 0) break;
                    quic_stream_on_max_stream_data(&conn->stream_ctx_, sid, md);
                    quic_conn_retry_send(conn);
                    break;
                }
                case QUIC_FRAME_RESET_STREAM: {
                    uint64_t sid, ec, fs;
                    consumed = quic_frame_parse_reset_stream(payload + pos,
                                                              payload_len - pos,
                                                              &sid, &ec, &fs);
                    if (consumed < 0) break;
                    quic_stream_on_reset(&conn->stream_ctx_, sid, fs);
                    break;
                }
                case QUIC_FRAME_STOP_SENDING: {
                    uint64_t sid, ec;
                    consumed = quic_frame_parse_stop_sending(payload + pos,
                                                              payload_len - pos,
                                                              &sid, &ec);
                    if (consumed < 0) break;
                    quic_stream_on_stop_sending(&conn->stream_ctx_, conn, sid,
                                                 quic_conn_queue_frames,
                                                 quic_conn_flush_packet);
                    break;
                }
                case QUIC_FRAME_MAX_STREAMS_BIDI:
                case QUIC_FRAME_MAX_STREAMS_UNI: {
                    uint64_t ms;
                    int bidi;
                    consumed = quic_frame_parse_max_streams(payload + pos,
                                                            payload_len - pos,
                                                            &ms, &bidi);
                    if (consumed < 0) break;
                    quic_stream_on_max_streams(&conn->stream_ctx_, ms, bidi);
                    break;
                }
                case QUIC_FRAME_DATA_BLOCKED: {
                    uint64_t md;
                    consumed = quic_frame_parse_data_blocked(payload + pos,
                                                             payload_len - pos, &md);
                    if (consumed < 0) break;
                    quic_stream_on_data_blocked(&conn->stream_ctx_, md);
                    break;
                }
                case QUIC_FRAME_STREAM_DATA_BLOCKED: {
                    uint64_t sid, msd;
                    consumed = quic_frame_parse_stream_data_blocked(payload + pos,
                                                                    payload_len - pos,
                                                                    &sid, &msd);
                    if (consumed < 0) break;
                    quic_stream_on_stream_data_blocked(&conn->stream_ctx_, sid, msd);
                    break;
                }
                case QUIC_FRAME_STREAMS_BLOCKED_BIDI:
                case QUIC_FRAME_STREAMS_BLOCKED_UNI: {
                    uint64_t ms;
                    int bidi;
                    consumed = quic_frame_parse_streams_blocked(payload + pos,
                                                                payload_len - pos,
                                                                &ms, &bidi);
                    if (consumed < 0) break;
                    quic_stream_on_streams_blocked(&conn->stream_ctx_, ms, bidi);
                    break;
                }
                case QUIC_FRAME_NEW_CONNECTION_ID: {
                    uint64_t seq, retire;
                    uint8_t cid_len;
                    const uint8_t *ncid_data, *ncid_token;
                    consumed = quic_frame_parse_new_conn_id(payload + pos,
                                                            payload_len - pos,
                                                            &seq, &retire, &cid_len,
                                                            &ncid_data, &ncid_token);
                    if (consumed > 0 && cid_len > 0 && cid_len <= QUIC_CID_MAX_LEN) {
                        /* retire_prior_to 要求我们将 seq < retire 的旧 CID 全部弃用 */
                        conn->retire_prior_to_ = retire;

                        /* 发送 RETIRE_CONNECTION_ID 退旧 CID */
                        if (conn->dst_cid_seq_ < seq) {
                            uint8_t frm[16];
                            int flen = quic_frame_write_retire_conn_id(
                                frm, sizeof(frm), conn->dst_cid_seq_);
                            if (flen > 0) {
                                quic_conn_queue_frames(conn, frm, (size_t)flen);
                                LOG_DEBUG("[quic-conn] retiring old dst_cid seq=%llu",
                                         (unsigned long long)conn->dst_cid_seq_);
                            }
                        }

                        /* 存储 Stateless Reset Token (RFC 9000 §10.3) */
                        if (conn->reset_token_count_ < QUIC_MAX_RESET_TOKENS) {
                            memcpy(conn->reset_tokens_[conn->reset_token_count_],
                                   ncid_token, 16);
                            conn->reset_token_count_++;
                        }

                        /* 采用新 CID */
                        memcpy(conn->dst_cid_.data, ncid_data, cid_len);
                        conn->dst_cid_.len = cid_len;
                        conn->dst_cid_seq_ = seq;
                        LOG_DEBUG("[quic-conn] adopted new dst_cid seq=%llu len=%u",
                                 (unsigned long long)seq, cid_len);
                    }
                    break;
                }
                case QUIC_FRAME_RETIRE_CONNECTION_ID: {
                    uint64_t rseq;
                    consumed = quic_frame_parse_retire_conn_id(payload + pos,
                                                               payload_len - pos, &rseq);
                    if (consumed > 0) {
                        LOG_DEBUG("[quic-conn] peer retired CID seq=%llu"
                                 " (our src_cid)", (unsigned long long)rseq);
                        /* 对端停用了我们发送的某个 src_cid。
                         * 简单实现：如果 retire 了我们当前的 CID，
                         * 需要协商新 CID（当前只记录日志） */
                    }
                    break;
                }
                case QUIC_FRAME_NEW_TOKEN: {
                    consumed = quic_frame_parse_new_token(payload + pos,
                                                          payload_len - pos);
                    if (consumed < 0) consumed = 1;
                    break;
                }
                default:
                {
                    char hex[128]; size_t hp = 0;
                    for (size_t h = pos; h < payload_len && hp < sizeof(hex)-3; h++)
                        hp += snprintf(hex+hp, sizeof(hex)-hp, "%02x", payload[h]);
                    LOG_WARN("[quic-conn] unknown frame 0x%02x pos=%zu/%zu bytes=%s",
                             ftype, pos, payload_len, hex);
                }
                    consumed = 1; /* 跳过 1 字节，继续尝试后续帧 */
                    break;
                }
                {
                    size_t advance = (consumed > 0) ? (size_t)consumed : 1;
                    pos += advance;
                }
            }
done_short:
            quic_conn_queue_ack(conn);

            /* 推进到下一个 coalesced 短头包。
             * 短头包总长 = header(pn_offset) + pn_len + 密文(pt_len+16 tag)。
             * 缺失此推进时，coalesced 的多个短头包（如 pending_1rtt 回放
             * 拼接的 91 字节 = 60B + 31B 两个包）会被当成一个包重复解密
             * → decrypt failed len=91。 */
            if (keys_ok) {
                size_t total = pn_offset + pn_len + pt_len + 16;
                if (total <= len) {
                    data += total;
                    len  -= total;
                    continue;
                }
            }
            break;
        }
    }
    } /* while (len > 0) */

    /* 握手读密钥刚就绪 + 有待回放缓存的 Handshake 包 →
     * 通过 goto 在同一栈帧内线性回放，完全避免递归/定时器重入。 */
    if (!conn->handshake_done_ && conn->pending_hs_len_ > 0 &&
        conn->keys_[QUIC_TLS_LEVEL_HANDSHAKE][0].initialized) {
        LOG_DEBUG("[quic-conn] replaying %zu bytes buffered Handshake pkts (goto)",
                 conn->pending_hs_len_);
        uint8_t *replay = conn->pending_hs_data_;
        size_t   rlen   = conn->pending_hs_len_;
        conn->pending_hs_data_ = NULL;
        conn->pending_hs_len_  = 0;
        conn->pending_hs_cap_  = 0;
        data = replay;
        len  = rlen;
        replay_buf_to_free = replay;  /* 记录原始指针，while 循环中 data 会偏移 */
        goto feed_replay;
    }

    /* app read 密钥已就绪 + 有待回放的 1-RTT 短头包 → 同上回放 */
    if (conn->keys_[QUIC_TLS_LEVEL_APPLICATION][0].initialized &&
        conn->pending_1rtt_len_ > 0) {
        LOG_DEBUG("[quic-conn] replaying %zu bytes buffered 1-RTT pkts (goto)",
                 conn->pending_1rtt_len_);
        uint8_t *replay = conn->pending_1rtt_data_;
        size_t   rlen   = conn->pending_1rtt_len_;
        conn->pending_1rtt_data_ = NULL;
        conn->pending_1rtt_len_  = 0;
        conn->pending_1rtt_cap_  = 0;
        data = replay;
        len  = rlen;
        replay_buf_to_free = replay;
        goto feed_replay;
    }

    if (replay_buf_to_free)
        free(replay_buf_to_free);
}

/* ============================================
 * Stream API（对上层暴露的包装）
 * ============================================ */

uint64_t QuicConnectionStreamOpen(QuicConnection *conn) {
    return quic_stream_open(&conn->stream_ctx_, conn, quic_conn_queue_frames);
}

uint64_t QuicConnectionStreamOpenUni(QuicConnection *conn) {
    return quic_stream_open_uni(&conn->stream_ctx_, conn, quic_conn_queue_frames);
}

int QuicConnectionStreamSend(QuicConnection *conn, uint64_t stream_id,
                             const uint8_t *data, size_t len, int fin) {
    return quic_stream_send(&conn->stream_ctx_, conn, stream_id,
                             data, len, fin,
                             quic_conn_queue_frames, quic_conn_flush_packet);
}

int QuicConnectionStreamSendEx(QuicConnection *conn, uint64_t stream_id,
                                const uint8_t *data, size_t len, int fin,
                                quic_stream_write_cb cb, void *user_data,
                                uint64_t timeout_ms) {
    return quic_stream_write(&conn->stream_ctx_, conn, stream_id,
                              data, len, fin, cb, user_data, timeout_ms,
                              quic_conn_queue_frames, quic_conn_flush_packet);
}

int QuicConnectionStreamSetOnWritable(QuicConnection *conn, uint64_t stream_id,
                                       void (*cb)(QuicStream *s, void *user),
                                       void *user) {
    if (!conn) return -1;
    return quic_stream_set_on_writable(&conn->stream_ctx_, stream_id, cb, user);
}

/* 直接 UDP 发送 — 不经过队列/CWND/flush timer。重传专用。 */
int QuicConnectionSendImmediate(void *vconn, int pkt_type,
                                 const uint8_t *data, size_t len) {
    QuicConnection *conn = (QuicConnection*)vconn;
    if (!conn) return -1;
    /* pkt_type=-1→短头(应用), QUIC_PKT_HANDSHAKE→长头(Handshake密钥) */
    return send_quic_packet(conn, pkt_type, data, len);
}

void QuicConnectionStreamCloseSend(QuicConnection *conn, uint64_t stream_id) {
    quic_stream_close_send(&conn->stream_ctx_, conn, stream_id,
                            quic_conn_queue_frames, quic_conn_flush_packet);
}

int QuicConnectionStreamReset(QuicConnection *conn, uint64_t stream_id,
                              uint64_t error_code) {
    return quic_stream_reset(&conn->stream_ctx_, conn, stream_id, error_code,
                              quic_conn_queue_frames, quic_conn_flush_packet);
}

int QuicConnectionStreamStopSending(QuicConnection *conn, uint64_t stream_id,
                                    uint64_t error_code) {
    return quic_stream_stop_sending(&conn->stream_ctx_, conn, stream_id,
                                     error_code,
                                     quic_conn_queue_frames,
                                     quic_conn_flush_packet);
}

void QuicConnectionSetOnStreamData(QuicConnection *conn,
                                   QuicConnectionOnStreamData cb) {
    conn->stream_ctx_.on_stream_data = cb;
}

/* ============================================
 * 构造 / 析构
 * ============================================ */

static void on_idle_timer(void *user) {
    QuicConnection *conn = (QuicConnection*)user;
    if (!conn || conn->freeing_ || conn->state_ >= QUIC_STATE_CLOSING) return;
    LOG_INFO("[quic-conn] idle timeout after %llums, closing connection",
             (unsigned long long)conn->idle_timeout_ms_);
    QuicConnectionClose(conn, QUIC_ERR_IDLE_TIMEOUT, "idle timeout");
}

static void arm_idle_timer(QuicConnection *conn) {
    if (conn->idle_timeout_ms_ > 0) {
        quic_timer_start(&conn->idle_timer_, on_idle_timer, conn,
                         conn->idle_timeout_ms_, 0);
    }
    arm_keepalive_timer(conn);
}

static void on_keepalive_timer(void *user) {
    QuicConnection *conn = (QuicConnection*)user;
    if (!conn || conn->freeing_ || conn->state_ >= QUIC_STATE_CLOSING) return;
    if (!conn->keepalive_enabled_) return;
    /* 只在 idle 时发送 PING（timer 只会在空闲时触发） */
    LOG_DEBUG("[quic-conn] keepalive: sending PING");
    QuicConnectionPing(conn);
}

/* ── DPLPMTUD Probe (RFC 8899) ─────────────── */

static void on_pmtu_probe(void *user) {
    QuicConnection *conn = (QuicConnection*)user;
    if (!conn || conn->freeing_ || conn->state_ < QUIC_STATE_ESTABLISHED) return;

    /* If previous probe never got ACKed → loss → stop at current size */
    if (conn->probe_size_ > 0) {
        LOG_DEBUG("[quic-conn] PMTU probe at %llu",
                 (unsigned long long)conn->probe_size_,
                 (unsigned long long)conn->current_pmtu_);
        conn->probe_size_ = 0;
        /* Stop further probing — current_pmtu_ is the max */
        return;
    }

    uint64_t next = conn->current_pmtu_ + QUIC_PMTU_PROBE_STEP;
    if (next > QUIC_PMTU_MAX) next = QUIC_PMTU_MAX;
    if (next <= conn->current_pmtu_) return;   /* at max already */

    conn->probe_size_ = next;
    LOG_DEBUG("[quic-conn] PMTU probing %llu → %llu",
              (unsigned long long)conn->current_pmtu_,
              (unsigned long long)next);

    /* Send a PING to trigger a probe-sized packet */
    QuicConnectionPing(conn);
}

static void arm_keepalive_timer(QuicConnection *conn) {
    if (conn->keepalive_enabled_ && conn->idle_timeout_ms_ > 0 &&
        conn->state_ < QUIC_STATE_CLOSING) {
        uint64_t interval = conn->idle_timeout_ms_ / 2;
        if (interval < 1000) interval = 1000; /* 最小 1s */
        quic_timer_start(&conn->keepalive_timer_, on_keepalive_timer, conn, interval, 0);
    }
}

/* ============================================
 * 主动 Key Update (RFC 9001 §6)
 * ============================================ */

/* TODO: 主动 Key Update。当前只用被动追随模式（对端换 phase 时自动跟上）。
 * 重新启用时：取消注释并实现 ACK 确认逻辑，防止双方同时发起造成振荡。 */
static void maybe_initiate_key_update(QuicConnection *conn) {
    (void)conn;
}

/* ── PTO keepalive: 补发 MAX_DATA + 各 stream MAX_STREAM_DATA ── */
static void pto_keepalive_cb(void *vconn) {
    QuicConnection *conn = (QuicConnection*)vconn;
    if (!conn || conn->freeing_ || conn->state_ >= QUIC_STATE_CLOSING) return;
    QuicStreamCtx *ctx = &conn->stream_ctx_;
    uint8_t frm[32];
    int flen;

    /* MAX_DATA（连接级） */
    flen = quic_frame_write_max_data(frm, sizeof(frm), ctx->max_data_local);
    if (flen > 0) quic_conn_queue_frames(conn, frm, (size_t)flen);

    /* MAX_STREAM_DATA（每个活跃 stream 级）
     * 防止 stream_fc_update 发出的 credit 被 proxy 丢包后死锁。 */
    for (size_t i = 0; i < ctx->stream_cnt; i++) {
        QuicStream *s = ctx->streams[i];
        if (!s || s->recv_state == QUIC_STREAM_RECV_RESET_RECVD) continue;
        flen = quic_frame_write_max_stream_data(frm, sizeof(frm),
                                                 s->stream_id,
                                                 s->recv_max_data);
        if (flen > 0) quic_conn_queue_frames(conn, frm, (size_t)flen);
    }

    quic_conn_flush_packet(conn);
    LOG_DEBUG("[quic-conn] PTO keepalive: re-send flow control credits"
             " (%zu streams)", ctx->stream_cnt);
}

static void recovery_conn_dead_cb(void *vconn) {
    QuicConnection *conn = (QuicConnection*)vconn;
    if (!conn || conn->freeing_) return;
    LOG_WARN("[quic-conn] recovery reports connection dead (max PTO exceeded)");
    QuicConnectionClose(conn, QUIC_ERR_NO_VIABLE_PATH, "no viable path");
}

static void on_close_timer(void *user) {
    QuicConnection *conn = (QuicConnection*)user;
    if (!conn || conn->freeing_) return;
    conn->state_ = QUIC_STATE_CLOSED;

    QuicConnectionOnClose cb = conn->on_close_;
    conn->on_close_ = NULL;
    if (cb) {
        cb(conn, conn->close_ec_, conn->close_reason_);
    }

    QuicConnectionDestruct(conn);
}

QuicConnection* QuicConnectionCreate(uv_loop_t *loop) {
    quic_crypto_init();

    if (!loop) {
        LOG_ERROR("[quic-conn] Create: loop is NULL");
        return NULL;
    }

    QuicConnection *conn = (QuicConnection*)calloc(1, sizeof(QuicConnection));
    if (!conn) {
        LOG_ERROR("[quic-conn] Create: calloc(%zu) failed", sizeof(QuicConnection));
        return NULL;
    }

    conn->loop_ = loop;
    conn->state_ = QUIC_STATE_INIT;

    /* 随机 CID */
    RAND_bytes(conn->src_cid_.data, QUIC_CID_LEN);
    conn->src_cid_.len = QUIC_CID_LEN;
    RAND_bytes(conn->dst_cid_.data, QUIC_CID_LEN); /* 客户端临时，Accept 时覆盖 */
    conn->dst_cid_.len = QUIC_CID_LEN;

    /* TLS 接收缓冲 */
    for (int i = 0; i < QUIC_TLS_LEVEL_NUM; i++) {
        conn->tls_rcap_[i] = TLS_BUF_SIZE;
        conn->tls_rbuf_[i] = (uint8_t*)malloc(TLS_BUF_SIZE);
        conn->tls_fill_[i] = (uint8_t*)calloc(1, TLS_BM_BYTES);
        if (!conn->tls_rbuf_[i] || !conn->tls_fill_[i]) {
            LOG_ERROR("[quic-conn] Create: TLS recv buf[%d] alloc failed", i);
            goto fail;
        }
    }

    /* SSL_CTX — 客户端默认，服务端在 Accept 时重新创建 */
    conn->ssl_ctx_ = tls_ctx_client_new();
    if (!conn->ssl_ctx_) {
        LOG_ERROR("[quic-conn] Create: tls_ctx_client_new failed");
        goto fail;
    }
    tls_ctx_set_alpn(conn->ssl_ctx_);

    conn->ssl_ = SSL_new(conn->ssl_ctx_);
    if (!conn->ssl_) {
        LOG_ERROR("[quic-conn] Create: SSL_new failed");
        goto fail;
    }

    /* 构造客户端 transport parameters (RFC 9000 §18) */
    {
        uint8_t *tp_buf = conn->local_tp_;
        size_t tp_pos = 0;

        /* initial_source_connection_id (0x0f) — 客户端 SCID */
        tp_pos += quic_varint_encode(tp_buf + tp_pos,
                                      sizeof(conn->local_tp_) - tp_pos, 0x0f);
        tp_pos += quic_varint_encode(tp_buf + tp_pos,
                                      sizeof(conn->local_tp_) - tp_pos,
                                      conn->src_cid_.len);
        memcpy(tp_buf + tp_pos, conn->src_cid_.data, conn->src_cid_.len);
        tp_pos += conn->src_cid_.len;

        /* initial_max_data (0x04) — 连接级流控 */
        {
            uint64_t val = QUIC_STREAM_RECV_BUF_SIZE;
            tp_pos += quic_varint_encode(tp_buf + tp_pos,
                                          sizeof(conn->local_tp_) - tp_pos, 0x04);
            tp_pos += quic_varint_encode(tp_buf + tp_pos,
                                          sizeof(conn->local_tp_) - tp_pos,
                                          quic_varint_len(val));
            tp_pos += quic_varint_encode(tp_buf + tp_pos,
                                          sizeof(conn->local_tp_) - tp_pos, val);
        }

        /* initial_max_stream_data_bidi_local (0x05) —
         * 服务端发起的 bidi stream 的 stream 级流控 */
        {
            uint64_t val = QUIC_STREAM_RECV_BUF_SIZE;
            tp_pos += quic_varint_encode(tp_buf + tp_pos,
                                          sizeof(conn->local_tp_) - tp_pos, 0x05);
            tp_pos += quic_varint_encode(tp_buf + tp_pos,
                                          sizeof(conn->local_tp_) - tp_pos,
                                          quic_varint_len(val));
            tp_pos += quic_varint_encode(tp_buf + tp_pos,
                                          sizeof(conn->local_tp_) - tp_pos, val);
        }

        /* initial_max_stream_data_bidi_remote (0x06) —
         * 客户端发起的 bidi stream 的 stream 级流控 */
        {
            uint64_t val = QUIC_STREAM_RECV_BUF_SIZE;
            tp_pos += quic_varint_encode(tp_buf + tp_pos,
                                          sizeof(conn->local_tp_) - tp_pos, 0x06);
            tp_pos += quic_varint_encode(tp_buf + tp_pos,
                                          sizeof(conn->local_tp_) - tp_pos,
                                          quic_varint_len(val));
            tp_pos += quic_varint_encode(tp_buf + tp_pos,
                                          sizeof(conn->local_tp_) - tp_pos, val);
        }

        /* initial_max_stream_data_uni (0x07) — 单向流 stream 级流控 */
        {
            uint64_t val = QUIC_STREAM_RECV_BUF_SIZE;
            tp_pos += quic_varint_encode(tp_buf + tp_pos,
                                          sizeof(conn->local_tp_) - tp_pos, 0x07);
            tp_pos += quic_varint_encode(tp_buf + tp_pos,
                                          sizeof(conn->local_tp_) - tp_pos,
                                          quic_varint_len(val));
            tp_pos += quic_varint_encode(tp_buf + tp_pos,
                                          sizeof(conn->local_tp_) - tp_pos, val);
        }

        /* max_idle_timeout (0x01) — 协商空闲超时 (ms) */
        {
            uint64_t val = conn->idle_timeout_ms_;
            tp_pos += quic_varint_encode(tp_buf + tp_pos,
                                          sizeof(conn->local_tp_) - tp_pos, 0x01);
            tp_pos += quic_varint_encode(tp_buf + tp_pos,
                                          sizeof(conn->local_tp_) - tp_pos,
                                          quic_varint_len(val));
            tp_pos += quic_varint_encode(tp_buf + tp_pos,
                                          sizeof(conn->local_tp_) - tp_pos, val);
        }

        /* disable_active_migration (0x0c) — 空值 TP，声明不支持连接迁移 */
        tp_pos += quic_varint_encode(tp_buf + tp_pos,
                                      sizeof(conn->local_tp_) - tp_pos, 0x0c);
        tp_pos += quic_varint_encode(tp_buf + tp_pos,
                                      sizeof(conn->local_tp_) - tp_pos, 0);

        /* reset_stream_at (0x1d) — quic-go 要求（空值） */
        tp_pos += quic_varint_encode(tp_buf + tp_pos,
                                      sizeof(conn->local_tp_) - tp_pos, 0x1d);
        tp_pos += quic_varint_encode(tp_buf + tp_pos,
                                      sizeof(conn->local_tp_) - tp_pos, 0);

        /* max_datagram_frame_size (0x20) — RFC 9221 §3 */
        if (conn->max_datagram_size_local_ > 0) {
            uint64_t mdfs = conn->max_datagram_size_local_;
            size_t vlen = quic_varint_len(mdfs);
            tp_pos += quic_varint_encode(tp_buf + tp_pos,
                                          sizeof(conn->local_tp_) - tp_pos, 0x20);
            tp_pos += quic_varint_encode(tp_buf + tp_pos,
                                          sizeof(conn->local_tp_) - tp_pos, vlen);
            tp_pos += quic_varint_encode(tp_buf + tp_pos,
                                          sizeof(conn->local_tp_) - tp_pos, mdfs);
        }

        conn->local_tp_len_ = tp_pos;
        /* 先设置 QUIC dispatch callbacks 启用 QUIC mode，再设置 TP */
        SSL_set_quic_tls_cbs(conn->ssl_, qtdis, conn);
        SSL_set_quic_tls_transport_params(conn->ssl_, tp_buf, tp_pos);
        LOG_DEBUG("[quic-conn] Create: set client TP (%zu bytes)", tp_pos);
    }

    quic_timer_wheel_init(loop);

    /* 关闭 / flush / idle / keepalive / PMTU：登记到 5ms 轮，无 per-conn uv_timer */
    conn->idle_timeout_ms_ = QUIC_IDLE_TIMEOUT_DEFAULT_MS;

    /* DATAGRAM — 默认启用，max 64KB */
    conn->max_datagram_size_peer_  = 0;
    conn->max_datagram_size_local_ = 65536;

    /* 保活计时器（默认关闭，QuicConnectionSetKeepalive 开启） */
    conn->keepalive_enabled_ = 0;

    /* DPLPMTUD 探测定时器 — 每 10s 尝试扩充 MTU */
    conn->current_pmtu_ = QUIC_PMTU_BASE;
    conn->probe_size_   = 0;
    conn->probe_pn_     = UINT64_MAX;
    quic_timer_start(&conn->pmtu_timer_, on_pmtu_probe, conn, QUIC_PMTU_PROBE_INTVL, 0);

    /* stream 层（客户端默认 bidi ID=0, uni ID=2；服务端在 Accept 中覆盖） */
    memset(&conn->stream_ctx_, 0, sizeof(conn->stream_ctx_));
    conn->stream_ctx_.next_local_bidi_id = 0;
    conn->stream_ctx_.next_local_uni_id  = 2;

    /* recovery 层 + 默认 NewReno 拥塞控制 */
    quic_recovery_init(&conn->recovery_ctx_, conn, loop,
                        quic_conn_queue_frames, quic_conn_flush_packet,
                        QuicConnectionSendImmediate,
                        NULL);  /* NULL → 默认 NewReno */
    conn->recovery_ctx_.on_connection_dead = recovery_conn_dead_cb;
    conn->recovery_ctx_.on_pto_keepalive   = pto_keepalive_cb;

    /* UDP recv buffer */
    conn->udp_rbuf_sz_ = 65536;
    conn->udp_rbuf_ = (uint8_t*)malloc(conn->udp_rbuf_sz_);
    if (!conn->udp_rbuf_) {
        LOG_ERROR("[quic-conn] Create: UDP recv buf malloc(%zu) failed", conn->udp_rbuf_sz_);
        goto fail;
    }

    return conn;

fail:
    QuicConnectionDestruct(conn);
    return NULL;
}

void QuicConnectionDestruct(QuicConnection *conn) {
    if (!conn || conn->freeing_) return;
    conn->freeing_ = 1;
    conn->state_ = QUIC_STATE_CLOSED;

    quic_timer_stop(&conn->close_timer_);
    quic_timer_stop(&conn->flush_timer_);
    quic_timer_stop(&conn->idle_timer_);
    quic_timer_stop(&conn->keepalive_timer_);
    quic_timer_stop(&conn->pmtu_timer_);
    quic_timer_stop(&conn->recovery_ctx_.pto_timer_);
    conn->recovery_ctx_.conn_ = NULL;

    quic_stream_cleanup(&conn->stream_ctx_);
    quic_recovery_cleanup(&conn->recovery_ctx_);

    conn->uv_close_left_ = 0;
    if (!conn->udp_shared_ && conn->udp_)
        conn_close_handle((uv_handle_t *)conn->udp_, conn);

    /* QuicTimer 嵌在 conn 上。心跳派发中可能还有本连接的其它 due 回调，
     * 必须等本轮 on_wheel 结束后再 free，否则 SIGSEGV in on_wheel。 */
    if (conn->uv_close_left_ == 0)
        quic_timer_defer_free(conn, conn_free_mem_cb);
}

/* ============================================
 * 回调设置
 * ============================================ */

void QuicConnectionSetOnConnected(QuicConnection *conn,
                                  QuicConnectionOnConnected cb) {
    conn->on_connected_ = cb;
}

void QuicConnectionSetOnClose(QuicConnection *conn,
                              QuicConnectionOnClose cb) {
    conn->on_close_ = cb;
}

/* ============================================
 * 客户端连接
 * ============================================ */

int QuicConnectionConnect(QuicConnection *conn, const char *ip, int port) {
    strncpy(conn->peer_ip_, ip, sizeof(conn->peer_ip_) - 1);
    conn->peer_port_ = port;

    /* 从临时 dst_cid_ (initial DCID) 派生 initial 密钥 */
    /* 客户端写 → client_initial_keys, 服务端写 → server_initial_keys */
    QuicCipherKeys client_ikeys, server_ikeys;
    if (quic_crypto_derive_initial_keys(&client_ikeys, &server_ikeys,
                                         conn->dst_cid_.data, conn->dst_cid_.len) < 0) {
        LOG_ERROR("[quic-conn] derive initial keys failed");
        return -1;
    }

    /* 客户端用 client_ikeys 写，用 server_ikeys 读 */
    conn->keys_[QUIC_TLS_LEVEL_NONE][0] = server_ikeys;    /* read */
    conn->keys_[QUIC_TLS_LEVEL_NONE][1] = client_ikeys;    /* write */

    /* 创建 UDP socket */
    conn->udp_ = (uv_udp_t*)malloc(sizeof(uv_udp_t));
    if (!conn->udp_) {
        LOG_ERROR("[quic-conn] Connect: malloc(uv_udp_t) failed");
        return -1;
    }
    uv_udp_init(conn->loop_, conn->udp_);
    conn->udp_->data = conn;
    conn->udp_shared_ = 0;

    struct sockaddr_in bind_addr;
    uv_ip4_addr("0.0.0.0", 0, &bind_addr);
    int ret = uv_udp_bind(conn->udp_, (const struct sockaddr*)&bind_addr, 0);
    if (ret < 0) {
        LOG_ERROR("[quic-conn] udp bind failed: %s", uv_strerror(ret));
        return ret;
    }

    ret = uv_udp_recv_start(conn->udp_, on_udp_alloc, on_udp_recv);
    if (ret < 0) {
        LOG_ERROR("[quic-conn] udp recv start failed: %s", uv_strerror(ret));
        return ret;
    }

    conn->state_ = QUIC_STATE_HANDSHAKE;
    LOG_DEBUG("[quic-conn] connecting to %s:%d", ip, port);
    arm_idle_timer(conn);

    /* 触发握手 — SSL_connect 会调用 crypto_send_cb */
    ret = SSL_connect(conn->ssl_);
    if (ret == 1) {
        /* 0-RTT 握手（极不可能在当前配置下） */
        conn->handshake_done_ = 1;
        conn->state_ = QUIC_STATE_ESTABLISHED;
        quic_crypto_set_cipher_suite_from_ssl(conn->ssl_);
        if (conn->on_connected_) conn->on_connected_(conn);
    } else {
        int err = SSL_get_error(conn->ssl_, ret);
        if (err != SSL_ERROR_WANT_READ) {
            LOG_ERROR("[quic-conn] SSL_connect failed: ret=%d ssl_err=%d", ret, err);
            tls_print_error("SSL_connect");
            return -1;
        }
        /* 握手未完成时，ClientHello 已登记进 recovery（R3），
         * PTO 会精确重传丢失的分片。 */
    }
    return 0;
}

/* ============================================
 * 关闭
 * ============================================ */

void QuicConnectionClose(QuicConnection *conn,
                         uint64_t error_code, const char *reason) {
    if (!conn || conn->state_ == QUIC_STATE_CLOSING
             || conn->state_ == QUIC_STATE_CLOSED) return;

    conn->close_ec_ = error_code;
    if (reason) {
        strncpy(conn->close_reason_, reason, sizeof(conn->close_reason_) - 1);
    }

    LOG_INFO("[quic-conn] closing: err=0x%llx reason=%s",
           (unsigned long long)error_code, reason ? reason : "");

    /* 发送 CONNECTION_CLOSE 帧 */
    uint8_t frame[512];
    size_t rlen = reason ? strlen(reason) : 0;
    int flen = quic_frame_write_connection_close(frame, sizeof(frame),
                                                  error_code, reason, rlen);
    if (flen > 0) {
        /* 使用 available 的最高保护级别发送 */
        int level = QUIC_TLS_LEVEL_APPLICATION;
        if (!conn->keys_[QUIC_TLS_LEVEL_APPLICATION][1].initialized) {
            level = QUIC_TLS_LEVEL_HANDSHAKE;
            if (!conn->keys_[QUIC_TLS_LEVEL_HANDSHAKE][1].initialized) {
                level = QUIC_TLS_LEVEL_NONE;
            }
        }

        QuicCipherKeys *wkeys = &conn->keys_[level][1];
        if (!wkeys->initialized) {
            LOG_ERROR("[quic-conn] Close: no write keys at level=%d, cannot send"
                      " CONNECTION_CLOSE", level);
            return;
        }

        uint8_t pkt[QUIC_MAX_PKT_SIZE];
        size_t pkt_len;
        uint64_t pn = conn->next_pn_[level]++;

        int pkt_type;
        if (level == QUIC_TLS_LEVEL_APPLICATION) pkt_type = -1;
        else if (level == QUIC_TLS_LEVEL_HANDSHAKE) pkt_type = QUIC_PKT_HANDSHAKE;
        else pkt_type = QUIC_PKT_INITIAL;

        int ret;
        if (pkt_type >= 0) {
            ret = quic_packet_build_long(pkt, &pkt_len, pkt_type,
                                          &conn->src_cid_, &conn->dst_cid_,
                                          NULL, 0, pn, frame, (size_t)flen, wkeys);
        } else {
            ret = quic_packet_build_short(pkt, &pkt_len, &conn->dst_cid_,
                                           pn, frame, (size_t)flen, wkeys,
                                           conn->current_key_phase_,
                                           conn->spin_bit_);
        }
        if (ret == 0) {
            do_udp_send(conn, pkt, pkt_len);
        }
    }

    conn->state_ = QUIC_STATE_CLOSING;

    /* 进入 CLOSING 后只留 close_timer，避免 1s 后 close 与 flush/idle 同拍到期 */
    quic_timer_stop(&conn->flush_timer_);
    quic_timer_stop(&conn->idle_timer_);
    quic_timer_stop(&conn->keepalive_timer_);
    quic_timer_stop(&conn->pmtu_timer_);
    quic_timer_stop(&conn->recovery_ctx_.pto_timer_);

    /* 1 秒后正式关闭 */
    quic_timer_start(&conn->close_timer_, on_close_timer, conn, 1000, 0);
}

/* ============================================
 * 服务端接受（listener 调用）
 * ============================================ */

int QuicConnectionAccept(QuicConnection *conn,
                         SSL_CTX *ssl_ctx,
                         uv_udp_t *shared_udp,
                         const char *client_ip, int client_port,
                         const QuicConnectionId *client_scid,
                         const QuicConnectionId *client_initial_dcid,
                         const QuicCipherKeys *client_read_keys,
                         const QuicCipherKeys *server_write_keys,
                         const uint8_t *initial_data, size_t initial_len,
                         int do_ssl_accept) {
    strncpy(conn->peer_ip_, client_ip, sizeof(conn->peer_ip_) - 1);
    conn->peer_port_ = client_port;

    /* 服务端 CID */
    quic_cid_copy(&conn->src_cid_, client_initial_dcid);
    /* 对端 CID */
    quic_cid_copy(&conn->dst_cid_, client_scid);

    /* 共享 UDP */
    conn->udp_ = shared_udp;
    conn->udp_shared_ = 1;

    /* 使用 listener 预派生的密钥（避免 DCID 截断问题） */
    conn->keys_[QUIC_TLS_LEVEL_NONE][0] = *client_read_keys;   /* 服务端 read */
    conn->keys_[QUIC_TLS_LEVEL_NONE][1] = *server_write_keys;  /* 服务端 write */

    /* 复用 listener 的 SSL_CTX */
    SSL_free(conn->ssl_);
    tls_ctx_free(conn->ssl_ctx_);
    conn->ssl_ctx_ = ssl_ctx;
    conn->ssl_ctx_shared_ = 1;

    conn->ssl_ = SSL_new(conn->ssl_ctx_);
    if (!conn->ssl_) {
        LOG_ERROR("[quic-conn] Accept: SSL_new failed");
        return -1;
    }
    SSL_set_accept_state(conn->ssl_);

    /* 构造 transport parameters (RFC 9000 §18) */
    /* 必须包含 original_destination_connection_id (0x00) */
    {
        uint8_t *tp_buf = conn->local_tp_;
        size_t tp_pos = 0;

        /* original_destination_connection_id (0x00) — 客户端原始 DCID */
        if (client_initial_dcid->len > 0) {
            tp_pos += quic_varint_encode(tp_buf + tp_pos,
                                          sizeof(conn->local_tp_) - tp_pos, 0x00);
            tp_pos += quic_varint_encode(tp_buf + tp_pos,
                                          sizeof(conn->local_tp_) - tp_pos,
                                          client_initial_dcid->len);
            memcpy(tp_buf + tp_pos, client_initial_dcid->data,
                   client_initial_dcid->len);
            tp_pos += client_initial_dcid->len;
        }

        /* initial_source_connection_id (0x0f) — 服务端 SCID */
        if (conn->src_cid_.len > 0) {
            tp_pos += quic_varint_encode(tp_buf + tp_pos,
                                          sizeof(conn->local_tp_) - tp_pos, 0x0f);
            tp_pos += quic_varint_encode(tp_buf + tp_pos,
                                          sizeof(conn->local_tp_) - tp_pos,
                                          conn->src_cid_.len);
            memcpy(tp_buf + tp_pos, conn->src_cid_.data, conn->src_cid_.len);
            tp_pos += conn->src_cid_.len;
        }

        /* initial_max_streams_bidi (0x08) — 允许客户端打开 100 个 bidi stream */
        {
            uint64_t val = 100;
            size_t vlen = quic_varint_len(val);
            tp_pos += quic_varint_encode(tp_buf + tp_pos,
                                          sizeof(conn->local_tp_) - tp_pos, 0x08);
            tp_pos += quic_varint_encode(tp_buf + tp_pos,
                                          sizeof(conn->local_tp_) - tp_pos, vlen);
            tp_pos += quic_varint_encode(tp_buf + tp_pos,
                                          sizeof(conn->local_tp_) - tp_pos, val);
        }

        /* initial_max_streams_uni (0x09) — 允许客户端打开 100 个 uni stream */
        {
            uint64_t val = 100;
            size_t vlen = quic_varint_len(val);
            tp_pos += quic_varint_encode(tp_buf + tp_pos,
                                          sizeof(conn->local_tp_) - tp_pos, 0x09);
            tp_pos += quic_varint_encode(tp_buf + tp_pos,
                                          sizeof(conn->local_tp_) - tp_pos, vlen);
            tp_pos += quic_varint_encode(tp_buf + tp_pos,
                                          sizeof(conn->local_tp_) - tp_pos, val);
        }

        /* initial_max_data (0x04) — 连接级流控 */
        {
            uint64_t val = QUIC_STREAM_RECV_BUF_SIZE;
            size_t vlen = quic_varint_len(val);
            tp_pos += quic_varint_encode(tp_buf + tp_pos,
                                          sizeof(conn->local_tp_) - tp_pos, 0x04);
            tp_pos += quic_varint_encode(tp_buf + tp_pos,
                                          sizeof(conn->local_tp_) - tp_pos, vlen);
            tp_pos += quic_varint_encode(tp_buf + tp_pos,
                                          sizeof(conn->local_tp_) - tp_pos, val);
        }

        /* initial_max_stream_data_bidi_remote (0x06) — 对端 bidi stream 流控 */
        {
            uint64_t val = QUIC_STREAM_RECV_BUF_SIZE;
            size_t vlen = quic_varint_len(val);
            tp_pos += quic_varint_encode(tp_buf + tp_pos,
                                          sizeof(conn->local_tp_) - tp_pos, 0x06);
            tp_pos += quic_varint_encode(tp_buf + tp_pos,
                                          sizeof(conn->local_tp_) - tp_pos, vlen);
            tp_pos += quic_varint_encode(tp_buf + tp_pos,
                                          sizeof(conn->local_tp_) - tp_pos, val);
        }

        /* initial_max_stream_data_bidi_local (0x05) — 本端发起的 bidi */
        {
            uint64_t val = QUIC_STREAM_RECV_BUF_SIZE;
            size_t vlen = quic_varint_len(val);
            tp_pos += quic_varint_encode(tp_buf + tp_pos,
                                          sizeof(conn->local_tp_) - tp_pos, 0x05);
            tp_pos += quic_varint_encode(tp_buf + tp_pos,
                                          sizeof(conn->local_tp_) - tp_pos, vlen);
            tp_pos += quic_varint_encode(tp_buf + tp_pos,
                                          sizeof(conn->local_tp_) - tp_pos, val);
        }

        /* initial_max_stream_data_uni (0x07) — 对端 uni 流控。
         * 缺省为 0：Chrome 能 createUnidirectionalStream，但写不出任何字节。 */
        {
            uint64_t val = QUIC_STREAM_RECV_BUF_SIZE;
            size_t vlen = quic_varint_len(val);
            tp_pos += quic_varint_encode(tp_buf + tp_pos,
                                          sizeof(conn->local_tp_) - tp_pos, 0x07);
            tp_pos += quic_varint_encode(tp_buf + tp_pos,
                                          sizeof(conn->local_tp_) - tp_pos, vlen);
            tp_pos += quic_varint_encode(tp_buf + tp_pos,
                                          sizeof(conn->local_tp_) - tp_pos, val);
        }

        /* max_idle_timeout (0x01) — 协商空闲超时 (ms) */
        {
            uint64_t val = conn->idle_timeout_ms_;
            size_t vlen = quic_varint_len(val);
            tp_pos += quic_varint_encode(tp_buf + tp_pos,
                                          sizeof(conn->local_tp_) - tp_pos, 0x01);
            tp_pos += quic_varint_encode(tp_buf + tp_pos,
                                          sizeof(conn->local_tp_) - tp_pos, vlen);
            tp_pos += quic_varint_encode(tp_buf + tp_pos,
                                          sizeof(conn->local_tp_) - tp_pos, val);
        }

        /* disable_active_migration (0x0c) — 空值 TP，声明不支持连接迁移 */
        tp_pos += quic_varint_encode(tp_buf + tp_pos,
                                      sizeof(conn->local_tp_) - tp_pos, 0x0c);
        tp_pos += quic_varint_encode(tp_buf + tp_pos,
                                      sizeof(conn->local_tp_) - tp_pos, 0);

        /* reset_stream_at (0x1d) — quic-go 要求（空值） */
        tp_pos += quic_varint_encode(tp_buf + tp_pos,
                                      sizeof(conn->local_tp_) - tp_pos, 0x1d);
        tp_pos += quic_varint_encode(tp_buf + tp_pos,
                                      sizeof(conn->local_tp_) - tp_pos, 0);

        /* max_datagram_frame_size (0x20) — RFC 9221 §3 */
        if (conn->max_datagram_size_local_ > 0) {
            uint64_t mdfs = conn->max_datagram_size_local_;
            size_t vlen = quic_varint_len(mdfs);
            tp_pos += quic_varint_encode(tp_buf + tp_pos,
                                          sizeof(conn->local_tp_) - tp_pos, 0x20);
            tp_pos += quic_varint_encode(tp_buf + tp_pos,
                                          sizeof(conn->local_tp_) - tp_pos, vlen);
            tp_pos += quic_varint_encode(tp_buf + tp_pos,
                                          sizeof(conn->local_tp_) - tp_pos, mdfs);
        }

        conn->local_tp_len_ = tp_pos;
        LOG_DEBUG("[quic-conn] tp: original_dcid=%uB initial_scid=%uB total=%zu",
                  client_initial_dcid->len, conn->src_cid_.len, tp_pos);
        SSL_set_quic_tls_cbs(conn->ssl_, qtdis, conn);
        if (!SSL_set_quic_tls_transport_params(conn->ssl_, tp_buf, tp_pos)) {
            LOG_ERROR("[quic-conn] SSL_set_quic_tls_transport_params failed");
        }
    }

    if (initial_len > 0) {
        int level = (int)conn->read_level_; /* 应该是 NONE (0) */
        memcpy(conn->tls_rbuf_[level] + conn->tls_rlen_[level],
               initial_data, initial_len);
        conn->tls_rlen_[level] += initial_len;
    }

    /* 服务端 bidi ID=1, uni ID=3 (RFC 9000 §2.1) */
    conn->stream_ctx_.next_local_bidi_id = 1;
    conn->stream_ctx_.next_local_uni_id  = 3;

    conn->state_ = QUIC_STATE_HANDSHAKE;
    arm_idle_timer(conn);

    /* 启动握手 — C↔C 单帧走 SSL_accept（完整数据），
     * curl/ngtcp2 多帧 gap 走 SSL_do_handshake（增量，contig 保护） */
    if (do_ssl_accept) {
        int ret = SSL_accept(conn->ssl_);
        if (ret == 1) {
            conn->handshake_done_ = 1;
            conn->state_ = QUIC_STATE_ESTABLISHED;
            quic_crypto_set_cipher_suite_from_ssl(conn->ssl_);
            LOG_DEBUG("[quic-conn] accept: handshake done");
            if (conn->on_connected_) conn->on_connected_(conn);
        } else {
            int err = SSL_get_error(conn->ssl_, ret);
            if (err != SSL_ERROR_WANT_READ) {
                LOG_DEBUG("[quic-conn] SSL_accept failed: ret=%d err=%d,"
                         " continuing with SSL_do_handshake",
                         ret, err);
                SSL_do_handshake(conn->ssl_);
            }
        }
    } else {
        /* gap 场景：延迟握手 — 不在此处调用任何 SSL 函数。
         * 调用方（listener）会在所有数据注入完成后
         * 手动触发 CryptoFlushHandshake。 */
        conn->handshake_deferred_ = 1;
        LOG_DEBUG("[quic-conn] accept: handshake deferred (do_ssl_accept=0)");
    }

    return 0;
}

/* ============================================
 * 访问器
 * ============================================ */

uv_loop_t* QuicConnectionGetLoop(QuicConnection *conn) { return conn->loop_; }
QuicState  QuicConnectionGetState(QuicConnection *conn) { return conn->state_; }
const QuicConnectionId* QuicConnectionGetSrcCid(QuicConnection *conn) { return &conn->src_cid_; }
const QuicConnectionId* QuicConnectionGetDstCid(QuicConnection *conn) { return &conn->dst_cid_; }

void QuicConnectionSetListener(QuicConnection *conn, void *listener) { conn->listener_ = listener; }
void* QuicConnectionGetListener(QuicConnection *conn) { return conn->listener_; }

void QuicConnectionSetAppData(QuicConnection *conn, void *data) { conn->app_data_ = data; }
void* QuicConnectionGetAppData(QuicConnection *conn) { return conn->app_data_; }

int QuicConnectionGetStats(QuicConnection *conn, QuicConnectionStats *out) {
    if (!conn || !out) return -1;
    memset(out, 0, sizeof(*out));

    uint64_t now = uv_now(conn->loop_);
    out->send_kbps = rate_kbps(conn->send_rate_start_ms_, conn->send_rate_bytes_,
                               conn->send_kbps_, now);
    out->recv_kbps = rate_kbps(conn->recv_rate_start_ms_, conn->recv_rate_bytes_,
                               conn->recv_kbps_, now);
    out->bytes_sent = conn->stats_bytes_sent_;
    out->bytes_recv = conn->stats_bytes_recv_;
    out->packets_sent = conn->stats_pkts_sent_;
    out->packets_recv = conn->stats_pkts_recv_;

    const QuicRecoveryCtx *rc = &conn->recovery_ctx_;
    out->packets_lost = rc->packets_lost_;
    out->packets_acked = rc->packets_acked_;
    out->srtt_ms = rc->smoothed_rtt_;
    out->latest_rtt_ms = rc->latest_rtt_;
    out->min_rtt_ms = rc->min_rtt_;
    out->rttvar_ms = rc->rttvar_;
    out->jitter_ms = rc->jitter_ms_;
    out->bytes_in_flight = rc->bytes_in_flight_;
    out->pto_count = rc->pto_count_;
    out->pto_ms = rc->pto_base_;

    if (rc->cc_ && rc->cc_->ops) {
        if (rc->cc_->ops->get_info) {
            struct quic_cc_info info;
            memset(&info, 0, sizeof(info));
            rc->cc_->ops->get_info(rc->cc_, &info);
            out->cwnd = info.cwnd;
            out->ssthresh = info.ssthresh;
            out->max_bw_kbps = info.max_bw_bps ? (info.max_bw_bps * 8) / 1000 : 0;
            out->cc_algo = info.algo;
            out->cc_state = info.state;
        } else if (rc->cc_->ops->get_cwnd) {
            out->cwnd = rc->cc_->ops->get_cwnd(rc->cc_);
        }
    }
    return 0;
}

void QuicConnectionSetIdleTimeout(QuicConnection *conn, uint64_t timeout_ms) {
    conn->idle_timeout_ms_ = timeout_ms;
}

void QuicConnectionSetKeepalive(QuicConnection *conn, int enabled) {
    conn->keepalive_enabled_ = enabled;
    if (enabled) {
        arm_keepalive_timer(conn);
    } else {
        quic_timer_stop(&conn->keepalive_timer_);
    }
}

void QuicConnectionPing(QuicConnection *conn) {
    if (conn->state_ >= QUIC_STATE_CLOSING) return;
    uint8_t ping = QUIC_FRAME_PING;
    quic_conn_queue_frames(conn, &ping, 1);
    quic_conn_flush_packet(conn);
}

void QuicConnectionResetRecoveryPto(QuicConnection *conn) {
    /* 仅重置 PTO 定时器和 inflight 计数，不清除丢包恢复能力。
     * 目的：CONNECT stream (stream 0) 的 pending crypto/stream 帧不触发
     *       虚假 PTO，但 WT 数据 stream (4, 8, …) 仍需要丢包重传。 */
    quic_recovery_reset_pto(&conn->recovery_ctx_);
    LOG_DEBUG("[quic-conn] recovery PTO reset after WT handshake");
}

/* ── DATAGRAM API (RFC 9221 §4) ────────────── */

int QuicConnectionSendDatagram(QuicConnection *conn,
                                const uint8_t *data, size_t len) {
    if (conn->state_ < QUIC_STATE_ESTABLISHED) return -1;
    if (conn->max_datagram_size_peer_ > 0 &&
        len > conn->max_datagram_size_peer_)
        return -2;

    uint8_t frame[2048];
    int flen = quic_frame_write_datagram(frame, sizeof(frame), data, len);
    if (flen < 0) return -1;

    /* 加入 flush 缓冲，通过 1ms 定时器批量发送 */
    quic_conn_queue_frames(conn, frame, (size_t)flen);
    return 0;
}

void QuicConnectionSetOnDatagram(QuicConnection *conn,
                                  QuicConnectionOnDatagram cb) {
    conn->on_datagram_ = cb;
}

void QuicConnectionSetMaxDatagramSizeLocal(QuicConnection *conn,
                                            uint64_t max_size) {
    conn->max_datagram_size_local_ = max_size;
}

/* 供 listener 调用 — CRYPTO 流有 gap 时限制连续范围
 *（Accept 默认全部连续，多帧 gap 场景需覆盖） */
void QuicConnectionSetCryptoContig(QuicConnection *conn, int level,
                                   uint64_t contig_end) {
    if (level >= 0 && level < QUIC_TLS_LEVEL_NUM)
        conn->tls_contig_[level] = (size_t)contig_end;
}

/* 供 listener 调用 — gap 场景增量写入 CRYPTO 数据 */
void QuicConnectionCryptoWriteGapped(QuicConnection *conn, int level,
                                      uint64_t off, const uint8_t *data, size_t len) {
    if (level < 0 || level >= QUIC_TLS_LEVEL_NUM) return;
    if (len == 0) return;

    size_t needed = (size_t)off + len;
    if (needed > conn->tls_rcap_[level]) {
        /* 扩容 */
        size_t new_cap = needed * 2;
        uint8_t *nb = (uint8_t*)realloc(conn->tls_rbuf_[level], new_cap);
        if (!nb) return;
        if (needed > conn->tls_rlen_[level])
            memset(nb + conn->tls_rlen_[level], 0, needed - conn->tls_rlen_[level]);
        conn->tls_rbuf_[level] = nb;
        conn->tls_rcap_[level] = new_cap;
    }

    /* 如果 offset 超出当前缓冲区，零填充 */
    if ((size_t)off > conn->tls_rlen_[level]) {
        size_t gap = (size_t)off - conn->tls_rlen_[level];
        if (conn->tls_rlen_[level] + gap > conn->tls_rcap_[level]) {
            size_t new_cap = (conn->tls_rlen_[level] + gap) * 2;
            uint8_t *nb = (uint8_t*)realloc(conn->tls_rbuf_[level], new_cap);
            if (!nb) return;
            conn->tls_rbuf_[level] = nb;
            conn->tls_rcap_[level] = new_cap;
        }
        memset(conn->tls_rbuf_[level] + conn->tls_rlen_[level], 0, gap);
        conn->tls_rlen_[level] += gap;
    }

    /* 写入数据 */
    size_t buf_off = (size_t)off;
    memcpy(conn->tls_rbuf_[level] + buf_off, data, len);
    if (buf_off + len > conn->tls_rlen_[level])
        conn->tls_rlen_[level] = buf_off + len;

    LOG_DEBUG("[quic-conn] crypto_write_gapped: level=%d off=%llu len=%zu rlen=%zu",
              level, (unsigned long long)off, len, conn->tls_rlen_[level]);
}

/* 供 listener 调用 — gap 场景循环驱动握手（当多帧 CRYPTO 有不连续偏移时）。
 * 从已写入 buffer 的 CRYPTO 帧中提取连续数据，直接喂给 OpenSSL */
void QuicConnectionCryptoFlushHandshake(QuicConnection *conn) {
    int level = 0;
    if (conn->handshake_done_) return;

    /* 清除延迟标记 — 允许后续 FeedRaw 正常调用 SSL_do_handshake */
    conn->handshake_deferred_ = 0;

    /* contig 覆盖所有已缓冲的连续数据 → 调用 SSL_accept */
    size_t rlen = conn->tls_rlen_[level];
    if (rlen == 0) return;

    /* 真实连续数据边界 = contig（bitmap扫描结果，不包含gap零填充）。
     * rlen 包含 gap 零填充 → 不能用 rlen 计算投喂边界。
     * 只在 bitmap 确认的全部连续数据≥上次已投喂的边界时才推进。 */
    size_t real_contig = conn->tls_contig_[level];
    if (real_contig <= conn->tls_contig_fed_[level] && !conn->crypto_send_pending_) {
        LOG_DEBUG("[quic-conn] crypto_flush skip: contig=%zu <= fed=%zu (gap or no new data)",
                 real_contig, conn->tls_contig_fed_[level]);
        return;
    }
    conn->tls_contig_fed_[level] = real_contig;
    conn->crypto_send_pending_ = 0;  /* 清除重试标志 */

    LOG_DEBUG("[quic-conn] crypto_flush: SSL_accept rlen=%zu cons=%zu contig=%zu fed=%zu",
             rlen, conn->tls_consumed_[level], conn->tls_contig_[level],
             conn->tls_contig_fed_[level]);

    int ret = SSL_accept(conn->ssl_);
    if (ret == 1) {
        conn->handshake_done_ = 1;
        conn->state_ = QUIC_STATE_ESTABLISHED;
        quic_crypto_set_cipher_suite_from_ssl(conn->ssl_);
        /* 服务端握手完成 → 清理 Initial 空间（Handshake 空间保留到
         * 首个 1-RTT 包，见 FeedRaw 短头分支） */
        quic_recovery_handshake_done(&conn->recovery_ctx_, 1);
        LOG_DEBUG("[quic-conn] crypto_flush: handshake done");
        if (conn->on_connected_) conn->on_connected_(conn);
        /* flush 握手完成后的应用层数据（SETTINGS/MAX_DATA 等 STREAM 帧）。
         * 否则这些帧留在 pkt_payload_，靠 1ms flush timer 发送，但该 timer
         * 受 cwnd 检查限制可能长期不发 → 客户端收不到 SETTINGS 死锁。 */
        quic_conn_flush_packet(conn);
    } else {
        int err = SSL_get_error(conn->ssl_, ret);
        LOG_DEBUG("[quic-conn] crypto_flush: SSL_accept ret=%d err=%d"
                 " (WANT_READ=%d) L0-contig=%zu/rlen=%zu/cons=%zu",
                 ret, err, SSL_ERROR_WANT_READ,
                 conn->tls_contig_[0], conn->tls_rlen_[0], conn->tls_consumed_[0]);
        /* SSL_accept 失败 → 重置 fed 标记。
         * retrans 到达后 contig 可能回到相同值，
         * fed=consumed 让下次调用能重新投喂刷新后的数据。 */
        conn->tls_contig_fed_[level] = conn->tls_consumed_[level];
    }
    /* 握手未完成 → 握手包已登记进 recovery（R3），PTO 精确重传接管，
     * 无需额外定时器。 */
}
