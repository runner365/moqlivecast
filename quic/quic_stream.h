#ifndef QUIC_STREAM_H
#define QUIC_STREAM_H

#include "quic_common.h"
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================
 * Stream 状态 (RFC 9000 §3)
 * ============================================ */

#define QUIC_STREAM_SEND_READY      0
#define QUIC_STREAM_SEND_SEND       1
#define QUIC_STREAM_SEND_DATA_SENT  2
#define QUIC_STREAM_SEND_RESET_SENT 3

#define QUIC_STREAM_RECV_RECV        0
#define QUIC_STREAM_RECV_SIZE_KNOWN  1
#define QUIC_STREAM_RECV_DATA_RECVD  2
#define QUIC_STREAM_RECV_RESET_RECVD 3

#define QUIC_STREAM_MAX_GAPS 8

typedef struct {
    uint64_t offset;
    uint64_t length;
} QuicStreamGap;

/* ── Write callback ───────────────────────────── */

struct QuicStream;
typedef void (*quic_stream_write_cb)(struct QuicStream *s,
                                     int ret_code,      /* 0=all acked, -1=conn close, -2=timeout, <-2=RESET */
                                     uint64_t length,    /* bytes acked */
                                     void *user_data);

/* ── Write request (linked list, ordered by stream_offset) ── */

typedef struct quic_stream_write_req {
    struct quic_stream_write_req *next;
    uint64_t              start_offset;   /* absolute stream offset */
    uint64_t              length;
    uint64_t              deadline_ms;    /* 0=no timeout, else uv_now() deadline */
    quic_stream_write_cb  cb;
    void                 *user_data;
} quic_stream_write_req_t;

/* ============================================
 * QuicStream 结构
 * ============================================ */

typedef struct QuicStream {
    uint64_t stream_id;

    /* 发送端 */
    int      send_state;
    uint64_t send_offset;         /* 下一字节发送偏移（已进入 wire/queue 的终点） */
    uint64_t send_max_data;       /* 对端给的 MAX_STREAM_DATA */

    /* 发送缓冲 */
    uint8_t *send_buf;            /* malloc'd buffer */
    size_t   send_buf_head;       /* 已发送待 memmove 的偏移 */
    size_t   send_buf_len;        /* 待发送字节数 (在 head 之后) */
    size_t   send_buf_cap;        /* 已分配总容量 */
    int      send_buf_fin;        /* 缓冲区中包含 FIN */
    int      send_blocked;        /* 1 = FC/CWND 挡住 consume */
    int      want_writable;       /* 1 = 曾因水位拒写，等低水位回调 */

    /* Write request 链表 — 按 start_offset 排序 */
    quic_stream_write_req_t *write_reqs;

    /* 发送水位回落时可写通知（可选） */
    void (*on_writable)(struct QuicStream *s, void *user);
    void *on_writable_user;

    /* 接收端 */
    int      recv_state;
    uint64_t recv_offset;
    uint64_t recv_max_data;
    uint8_t *recv_buf;
    size_t   recv_buf_len;
    size_t   recv_buf_cap;
    int      fin_recvd;
    uint64_t final_size;

    /* 乱序缓冲 */
    QuicStreamGap gaps[QUIC_STREAM_MAX_GAPS];
    int           gap_count;
} QuicStream;

/* ============================================
 * Stream 管理上下文（嵌入在 QuicConnection 中）
 * ============================================ */
typedef struct {
    QuicStream **streams;
    size_t       stream_cnt;
    size_t       stream_cap;
    uint64_t     next_local_bidi_id;
    uint64_t     next_local_uni_id;
    uint64_t     max_data_local;
    uint64_t     max_data_peer;
    uint64_t     bytes_sent;
    uint64_t     bytes_recvd;
    uint64_t     max_streams_bidi_peer;
    uint64_t     max_streams_uni_peer;
    uint64_t     max_streams_bidi_local;
    uint64_t     max_streams_uni_local;
    /* 对端 TP：initial_max_stream_data_{bidi_local,bidi_remote,uni} */
    uint64_t     peer_init_max_stream_data_bidi_local;
    uint64_t     peer_init_max_stream_data_bidi_remote;
    uint64_t     peer_init_max_stream_data_uni;
    QuicConnectionOnStreamData on_stream_data;
    int flushing_;  /* flush_pending 防重入 */
    int64_t last_quic_stream_dbg_ts_s;
} QuicStreamCtx;

/* ============================================
 * Stream 操作
 * ============================================ */

uint64_t quic_stream_open(QuicStreamCtx *ctx, void *conn,
                           int (*queue_fn)(void *conn, const uint8_t *frames, size_t flen));

uint64_t quic_stream_open_uni(QuicStreamCtx *ctx, void *conn,
                               int (*queue_fn)(void *conn, const uint8_t *frames, size_t flen));

/* 发送数据（带回调版本）。全部 ACK → cb(s, 0, len, user_data)。
 * timeout_ms=0 默认 5000ms。超时 → cb(s, -2, len, user_data)。
 * RESET/CONNECTION_CLOSE → cb(s, error_code, 0, user_data)。 */
int quic_stream_write(QuicStreamCtx *ctx, void *conn, uint64_t stream_id,
                       const uint8_t *data, size_t len, int fin,
                       quic_stream_write_cb cb, void *user_data,
                       uint64_t timeout_ms,
                       int (*queue_fn)(void *conn, const uint8_t *frames, size_t flen),
                       void (*flush_fn)(void *conn));

/* 兼容旧接口 — 等价于 quic_stream_write(..., NULL, NULL) */
int quic_stream_send(QuicStreamCtx *ctx, void *conn, uint64_t stream_id,
                      const uint8_t *data, size_t len, int fin,
                      int (*queue_fn)(void *conn, const uint8_t *frames, size_t flen),
                      void (*flush_fn)(void *conn));

void quic_stream_close_send(QuicStreamCtx *ctx, void *conn, uint64_t stream_id,
                             int (*queue_fn)(void *conn, const uint8_t *frames, size_t flen),
                             void (*flush_fn)(void *conn));

int quic_stream_reset(QuicStreamCtx *ctx, void *conn, uint64_t stream_id,
                       uint64_t error_code,
                       int (*queue_fn)(void *conn, const uint8_t *frames, size_t flen),
                       void (*flush_fn)(void *conn));

int quic_stream_stop_sending(QuicStreamCtx *ctx, void *conn, uint64_t stream_id,
                              uint64_t error_code,
                              int (*queue_fn)(void *conn, const uint8_t *frames, size_t flen),
                              void (*flush_fn)(void *conn));

int quic_stream_on_recv(QuicStreamCtx *ctx, void *conn,
                         uint64_t stream_id, uint64_t offset, int fin,
                         const uint8_t *data, size_t len,
                         int (*queue_fn)(void *conn, const uint8_t *frames, size_t flen),
                         void (*flush_fn)(void *conn));

void quic_stream_handshake_done(QuicStreamCtx *ctx, void *conn,
                                 int (*queue_fn)(void *conn, const uint8_t *frames, size_t flen));

void quic_stream_on_max_data(QuicStreamCtx *ctx, uint64_t max_data);
void quic_stream_on_max_stream_data(QuicStreamCtx *ctx, uint64_t stream_id, uint64_t max_data);
void quic_stream_apply_init_send_credit(QuicStreamCtx *ctx);
void quic_stream_on_max_streams(QuicStreamCtx *ctx, uint64_t max_streams, int bidi);

void quic_stream_on_reset(QuicStreamCtx *ctx, uint64_t stream_id, uint64_t final_size);
void quic_stream_on_stop_sending(QuicStreamCtx *ctx, void *conn, uint64_t stream_id,
                                  int (*queue_fn)(void *conn, const uint8_t *frames, size_t flen),
                                  void (*flush_fn)(void *conn));

void quic_stream_on_data_blocked(QuicStreamCtx *ctx, uint64_t max_data);
void quic_stream_on_stream_data_blocked(QuicStreamCtx *ctx,
                                         uint64_t stream_id, uint64_t limit);
void quic_stream_on_streams_blocked(QuicStreamCtx *ctx,
                                     uint64_t max_streams, int bidi);

void quic_stream_flush_pending(QuicStreamCtx *ctx, void *conn,
                                int (*queue_fn)(void *conn, const uint8_t *frames, size_t flen),
                                void (*flush_fn)(void *conn));

/* send_buf 水位：超过 high 时 quic_stream_write 返回 1（不接受本次数据） */
#ifndef QUIC_STREAM_SEND_HIGH_WATER
#define QUIC_STREAM_SEND_HIGH_WATER (512u * 1024u)
#endif
#ifndef QUIC_STREAM_SEND_LOW_WATER
#define QUIC_STREAM_SEND_LOW_WATER  (128u * 1024u)
#endif

/* quic_stream_write 返回值：0 已接受，1 拥塞（未接受），-1 异常 */
int  quic_stream_set_on_writable(QuicStreamCtx *ctx, uint64_t stream_id,
                                  void (*cb)(QuicStream *s, void *user), void *user);
size_t quic_stream_send_pending(QuicStreamCtx *ctx, uint64_t stream_id);

void quic_stream_cleanup(QuicStreamCtx *ctx);

#ifdef __cplusplus
}
#endif

#endif /* QUIC_STREAM_H */
