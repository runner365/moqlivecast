#ifndef QUIC_COMMON_H
#define QUIC_COMMON_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================
 * QUIC 版本
 * ============================================ */
#define QUIC_VERSION_V1  0x00000001

/* ============================================
 * 连接 ID (RFC 9000 — 支持可变长度，最大 20 字节)
 * ============================================ */
#define QUIC_CID_MAX_LEN  20
#define QUIC_CID_LEN       8  /* 保留用于向后兼容 */

typedef struct {
    uint8_t data[QUIC_CID_MAX_LEN];
    uint8_t len;  /* 实际使用的长度 (0–20) */
} QuicConnectionId;

/* ============================================
 * 包类型（长头第 1 字节低 2 位）
 * ============================================ */
#define QUIC_PKT_INITIAL     0x00
#define QUIC_PKT_ZERO_RTT    0x01
#define QUIC_PKT_HANDSHAKE   0x02
#define QUIC_PKT_RETRY       0x03

/* 长头标志位 */
#define QUIC_LONG_HEADER     0x80
#define QUIC_FIXED_BIT       0x40

/* ============================================
 * 帧类型 (RFC 9000 §19)
 * ============================================ */
#define QUIC_FRAME_PADDING             0x00
#define QUIC_FRAME_PING                0x01
#define QUIC_FRAME_ACK                 0x02
#define QUIC_FRAME_ACK_ECN             0x03
#define QUIC_FRAME_RESET_STREAM        0x04
#define QUIC_FRAME_STOP_SENDING        0x05
#define QUIC_FRAME_CRYPTO              0x06
#define QUIC_FRAME_NEW_TOKEN           0x07
#define QUIC_FRAME_STREAM              0x08  /* 0x08–0x0f */
#define QUIC_FRAME_MAX_DATA            0x10
#define QUIC_FRAME_MAX_STREAM_DATA     0x11
#define QUIC_FRAME_MAX_STREAMS_BIDI    0x12
#define QUIC_FRAME_MAX_STREAMS_UNI     0x13
#define QUIC_FRAME_DATA_BLOCKED        0x14
#define QUIC_FRAME_STREAM_DATA_BLOCKED 0x15
#define QUIC_FRAME_STREAMS_BLOCKED_BIDI  0x16
#define QUIC_FRAME_STREAMS_BLOCKED_UNI   0x17
#define QUIC_FRAME_NEW_CONNECTION_ID   0x18
#define QUIC_FRAME_RETIRE_CONNECTION_ID 0x19
#define QUIC_FRAME_PATH_CHALLENGE      0x1a
#define QUIC_FRAME_PATH_RESPONSE       0x1b
#define QUIC_FRAME_CONNECTION_CLOSE    0x1c
#define QUIC_FRAME_CONNECTION_CLOSE_APP 0x1d
#define QUIC_FRAME_HANDSHAKE_DONE      0x1e
#define QUIC_FRAME_DATAGRAM            0x30  /* RFC 9221 §4 — 不带长度 */
#define QUIC_FRAME_DATAGRAM_LEN        0x31  /* RFC 9221 §4 — 带长度 */

/* ============================================
 * 传输错误码 (RFC 9000 §20)
 * ============================================ */
#define QUIC_ERR_NO_ERROR              0x00
#define QUIC_ERR_INTERNAL              0x01
#define QUIC_ERR_CONNECTION_REFUSED    0x02
#define QUIC_ERR_FLOW_CONTROL          0x03
#define QUIC_ERR_STREAM_LIMIT          0x04
#define QUIC_ERR_STREAM_STATE          0x05
#define QUIC_ERR_FINAL_SIZE            0x06
#define QUIC_ERR_FRAME_ENCODING        0x07
#define QUIC_ERR_TRANSPORT_PARAM       0x08
#define QUIC_ERR_CONNECTION_ID         0x09
#define QUIC_ERR_PROTOCOL_VIOLATION    0x0a
#define QUIC_ERR_INVALID_TOKEN         0x0b
#define QUIC_ERR_APPLICATION           0x0c
#define QUIC_ERR_CRYPTO_BUFFER_EXCEEDED 0x0d
#define QUIC_ERR_KEY_UPDATE            0x0e
#define QUIC_ERR_AEAD_LIMIT_REACHED    0x0f
#define QUIC_ERR_NO_VIABLE_PATH        0x10
#define QUIC_ERR_IDLE_TIMEOUT          0x19
#define QUIC_ERR_CRYPTO                0x0100  /* + TLS alert code */

/* ============================================
 * 包大小
 * ============================================ */
#define QUIC_MIN_PKT_SIZE    1200
#define QUIC_MAX_PKT_SIZE    1500  /* 合理的发送上限 */
#define QUIC_MAX_PAYLOAD_LEN 1286   /* 新增：合包 payload 上限 = 目标整包1350 − 预留64 */

/* Initial/Handshake CRYPTO 帧分片上限。保证握手前 datagram ≤1200
 * (RFC 9000 §14.1 初始 PMTU)。client header 约 43B → payload ≤1157，
 * 留余量(PN/length varint/Retry token)取 1150。仅用于长头包分片。 */
#define QUIC_INITIAL_MAX_PAYLOAD 1150
#define QUIC_PMTU_BASE       1200
#define QUIC_PMTU_MAX        1350
#define QUIC_PMTU_PROBE_STEP  128
#define QUIC_PMTU_PROBE_INTVL 10000  /* ms — PMTUD probe interval */
#define QUIC_UDP_BUF_SIZE   65536 /* 接收缓冲区 */

/* ============================================
 * TLS 保护级别
 * ============================================ */
#define QUIC_TLS_LEVEL_NONE        0
#define QUIC_TLS_LEVEL_EARLY       1
#define QUIC_TLS_LEVEL_HANDSHAKE   2
#define QUIC_TLS_LEVEL_APPLICATION 3
#define QUIC_TLS_LEVEL_NUM         4

/* ============================================
 * 连接状态
 * ============================================ */
typedef enum {
    QUIC_STATE_INIT = 0,
    QUIC_STATE_HANDSHAKE,
    QUIC_STATE_ESTABLISHED,
    QUIC_STATE_CLOSING,
    QUIC_STATE_CLOSED
} QuicState;

/* ============================================
 * 包保护密钥 — 支持 AES-128/256 + ChaCha20
 * ============================================ */
typedef struct {
    uint8_t key[32];       /* AES-128:16, AES-256:32, ChaCha20:32 */
    uint8_t iv[12];        /* AEAD nonce 前缀 (固定 12 字节) */
    uint8_t hp_key[32];    /* header protection key (同 AEAD 密钥长度) */
    int     key_len;       /* 实际 key 长度: 16 or 32 */
    int     suite;         /* 创建时的 cipher suite (0/1/2) — 密钥终身有效 */
    int     initialized;
} QuicCipherKeys;

/* ============================================
 * ACK 帧结构
 * ============================================ */
#define QUIC_ACK_MAX_RANGES  32

typedef struct {
    uint64_t largest_acknowledged;
    uint64_t ack_delay;
    uint64_t first_ack_range;       /* 从 largest_acknowledged 向前的连续 ACK 数 */
    uint64_t gap[QUIC_ACK_MAX_RANGES];
    uint64_t ack_range[QUIC_ACK_MAX_RANGES];
    int      num_ranges;
} QuicAckFrame;

/* ============================================
 * 回调类型
 * ============================================ */
typedef struct QuicConnection QuicConnection;
typedef struct QuicListener   QuicListener;

typedef void (*QuicConnectionOnConnected)(QuicConnection *conn);
typedef void (*QuicConnectionOnClose)(QuicConnection *conn,
    uint64_t error_code, const char *reason);
typedef void (*QuicListenerOnConnection)(QuicListener *listener,
    QuicConnection *conn, const char *remote_ip, int remote_port);

typedef void (*QuicConnectionOnStreamData)(QuicConnection *conn,
    uint64_t stream_id, const uint8_t *data, size_t len, int fin);

typedef void (*QuicConnectionOnDatagram)(QuicConnection *conn,
    const uint8_t *data, size_t len);

/* ============================================
 * Stream 常量
 * ============================================ */
#define QUIC_STREAM_RECV_BUF_SIZE  (64UL * 1024 * 1024)  /* 64MB per-stream recv buffer */

/* ============================================
 * 工具宏
 * ============================================ */
#define QUIC_MIN(a, b)  ((a) < (b) ? (a) : (b))
#define QUIC_MAX(a, b)  ((a) > (b) ? (a) : (b))

static inline void quic_cid_copy(QuicConnectionId *dst, const QuicConnectionId *src) {
    memcpy(dst->data, src->data, src->len);
    dst->len = src->len;
}

static inline int quic_cid_eq(const QuicConnectionId *a, const QuicConnectionId *b) {
    return a->len == b->len && memcmp(a->data, b->data, a->len) == 0;
}

static inline void quic_cid_zero(QuicConnectionId *cid) {
    memset(cid->data, 0, sizeof(cid->data));
    cid->len = 0;
}

#ifdef __cplusplus
}
#endif

#endif /* QUIC_COMMON_H */
