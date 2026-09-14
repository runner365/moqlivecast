#ifndef QUIC_CONNECTION_H
#define QUIC_CONNECTION_H

#include "quic_common.h"
#include "quic_stream.h"
#include <uv.h>
#include <openssl/ssl.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================
 * QuicConnection — 客户端 / 服务端通用
 * ============================================ */

typedef struct QuicConnection QuicConnection;

/* 构造 — 客户端调用 */
QuicConnection* QuicConnectionCreate(uv_loop_t *loop);

/* 析构 — 触发异步关闭链 */
void QuicConnectionDestruct(QuicConnection *conn);

/* 回调设置 */
void QuicConnectionSetOnConnected(QuicConnection *conn,
                                  QuicConnectionOnConnected cb);
void QuicConnectionSetOnClose(QuicConnection *conn,
                              QuicConnectionOnClose cb);

/* 客户端 — 发起连接 */
int  QuicConnectionConnect(QuicConnection *conn,
                           const char *ip, int port);

/* 通用 — 关闭连接 */
void QuicConnectionClose(QuicConnection *conn,
                         uint64_t error_code, const char *reason);

/* ============================================
 * 以下 API 供 QuicListener 内部使用
 * ============================================ */

/* 服务端接受连接 — ssl_ctx/keys 复用 listener 已派生的 */
int  QuicConnectionAccept(QuicConnection *conn,
                          SSL_CTX *ssl_ctx,
                          uv_udp_t *shared_udp,
                          const char *client_ip, int client_port,
                          const QuicConnectionId *client_scid,
                          const QuicConnectionId *client_initial_dcid,
                          const QuicCipherKeys *client_read_keys,
                          const QuicCipherKeys *server_write_keys,
                          const uint8_t *initial_data, size_t initial_len,
                          int do_ssl_accept);

/* 入站数据包分发 */
void QuicConnectionFeedRaw(QuicConnection *conn,
                           const uint8_t *data, size_t len,
                           const char *from_ip, int from_port);

/* 反放大计数 — 服务端 listener 首包自解析路径（不走 FeedRaw）的入站字节。
 * RFC 9000 §8.1：服务端地址验证前，所有收到的字节都应计入 3× 预算，
 * 否则首包不计入会导致 ServerHello 被反放大拦截。 */
void QuicConnectionAccountRecvBytes(QuicConnection *conn, size_t len);

/* Stream API */
uint64_t QuicConnectionStreamOpen(QuicConnection *conn);
uint64_t QuicConnectionStreamOpenUni(QuicConnection *conn);  /* 单向 stream */
int      QuicConnectionStreamSend(QuicConnection *conn, uint64_t stream_id,
                                  const uint8_t *data, size_t len, int fin);

/* ── 带回调的 Stream 发送 ──────
 * 所有数据被对端 ACK 后触发 cb(s, 0, len, user_data)。
 * 失败时 cb(s, error_code, 0, user_data)。
 * cb 在 QUIC loop 线程中调用。
 * quic_stream_write_cb 类型定义在 quic_stream.h 中。 */
int      QuicConnectionStreamSendEx(QuicConnection *conn, uint64_t stream_id,
                                    const uint8_t *data, size_t len, int fin,
                                    quic_stream_write_cb cb, void *user_data,
                                    uint64_t timeout_ms);
/* 水位回落时可写回调；cb 在 QUIC loop 线程 */
int      QuicConnectionStreamSetOnWritable(QuicConnection *conn, uint64_t stream_id,
                                           void (*cb)(QuicStream *s, void *user),
                                           void *user);
void     QuicConnectionStreamCloseSend(QuicConnection *conn, uint64_t stream_id);
int      QuicConnectionStreamReset(QuicConnection *conn, uint64_t stream_id,
                                   uint64_t error_code);
int      QuicConnectionStreamStopSending(QuicConnection *conn, uint64_t stream_id,
                                         uint64_t error_code);
void     QuicConnectionSetOnStreamData(QuicConnection *conn,
                                       QuicConnectionOnStreamData cb);

/* 访问器 */
uv_loop_t*          QuicConnectionGetLoop(QuicConnection *conn);
QuicState           QuicConnectionGetState(QuicConnection *conn);
const QuicConnectionId* QuicConnectionGetSrcCid(QuicConnection *conn);
const QuicConnectionId* QuicConnectionGetDstCid(QuicConnection *conn);

/* 空闲超时（0 = 禁用，默认 30000ms，RFC 9000 §10.1） */
void QuicConnectionSetIdleTimeout(QuicConnection *conn, uint64_t timeout_ms);

/* 保活：自动在 idle_timeout/2 间隔发送 PING，防止对端空闲超时 */
void QuicConnectionSetKeepalive(QuicConnection *conn, int enabled);

/* 立即发送 PING 帧（RFC 9000 §19.2） */
void QuicConnectionPing(QuicConnection *conn);

/* DATAGRAM 帧 (RFC 9221 §4) — 不可靠数据报 */
int  QuicConnectionSendDatagram(QuicConnection *conn,
                                 const uint8_t *data, size_t len);
void QuicConnectionSetOnDatagram(QuicConnection *conn,
                                  QuicConnectionOnDatagram cb);
void QuicConnectionSetMaxDatagramSizeLocal(QuicConnection *conn,
                                            uint64_t max_size);

/* 重置恢复层 PTO（WebTransport CONNECT 后调用） */
void QuicConnectionResetRecoveryPto(QuicConnection *conn);

/* 立即 UDP 发送（绕过队列/CWND/flush timer，供重传使用） */
int  QuicConnectionSendImmediate(void *vconn, int pkt_type,
                                  const uint8_t *data, size_t len);

/* CRYPTO 流连续偏移跟踪 — 供 listener 在多帧 gap 场景下限制 crypto_recv 返回范围 */
void QuicConnectionSetCryptoContig(QuicConnection *conn, int level, uint64_t contig_end);

/* CRYPTO 流增量写入 + 握手中循环 — 供 listener 在 gap 场景使用 */
void QuicConnectionCryptoWriteGapped(QuicConnection *conn, int level,
                                      uint64_t off, const uint8_t *data, size_t len);
void QuicConnectionCryptoFlushHandshake(QuicConnection *conn);

/* Listener 反向引用（用于连接关闭时清理） */
void QuicConnectionSetListener(QuicConnection *conn, void *listener);
void* QuicConnectionGetListener(QuicConnection *conn);

/* 应用层数据指针（HTTP/3 server 用此获取 server 上下文） */
void QuicConnectionSetAppData(QuicConnection *conn, void *data);
void* QuicConnectionGetAppData(QuicConnection *conn);

/* ── 路径 / 拥塞快照（可随时查询，单位见字段） ── */
typedef struct QuicConnectionStats {
    uint64_t send_kbps;         /* last ~1s send, kilobits/s */
    uint64_t recv_kbps;         /* last ~1s recv, kilobits/s */
    uint64_t bytes_sent;
    uint64_t bytes_recv;
    uint64_t packets_sent;
    uint64_t packets_recv;
    uint64_t packets_lost;
    uint64_t packets_acked;

    uint64_t srtt_ms;
    uint64_t latest_rtt_ms;
    uint64_t min_rtt_ms;
    uint64_t rttvar_ms;
    uint64_t jitter_ms;

    uint64_t bytes_in_flight;
    uint64_t cwnd;
    uint64_t ssthresh;
    uint64_t max_bw_kbps;       /* BBR; 0 for NewReno */
    uint64_t pto_count;
    uint64_t pto_ms;
    int      cc_algo;           /* 1=NewReno, 2=BBR */
    int      cc_state;
} QuicConnectionStats;

/* 成功返回 0，conn/out 为空返回 -1 */
int QuicConnectionGetStats(QuicConnection *conn, QuicConnectionStats *out);

#ifdef __cplusplus
}
#endif

#endif /* QUIC_CONNECTION_H */
