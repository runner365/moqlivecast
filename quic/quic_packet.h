#ifndef QUIC_PACKET_H
#define QUIC_PACKET_H

#include "quic_common.h"
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================
 * Varint (RFC 9000 §16)
 * ============================================ */

/* 返回写入的字节数，buf 空间不足返回 -1 */
int      quic_varint_encode(uint8_t *buf, size_t cap, uint64_t val);

/* 返回消耗的字节数，非法值返回 -1 */
int      quic_varint_decode(const uint8_t *data, size_t len, uint64_t *val);

/* 返回 val 编码所需的字节数 (1/2/4/8) */
size_t   quic_varint_len(uint64_t val);

/* ============================================
 * Packet Number (RFC 9000 §17.1)
 * ============================================ */

/* 返回 PN 截断编码所需的字节数 (1/2/3/4) */
size_t   quic_pn_encode_len(uint64_t pn);

/* 将 PN 截断编码为 len 个字节 */
void     quic_pn_encode(uint8_t *buf, uint64_t pn, size_t len);

/* 从 len 字节还原 PN，expected 是期望的下一个 PN */
uint64_t quic_pn_decode(const uint8_t *buf, size_t len, uint64_t expected);

/* ============================================
 * 长头包 (Initial / Handshake) — RFC 9000 §17.2
 * ============================================ */

/* 构造长头包（含加密 + Header Protection）。
 * out 至少需要 QUIC_MAX_PKT_SIZE 字节。
 * 返回 0 成功，<0 失败（buffer 不足等）。 */
int quic_packet_build_long(uint8_t *out, size_t *out_len,
                           int pkt_type,          /* QUIC_PKT_INITIAL / QUIC_PKT_HANDSHAKE */
                           const QuicConnectionId *src_cid,
                           const QuicConnectionId *dst_cid,
                           const uint8_t *token, size_t token_len,
                           uint64_t pn,
                           const uint8_t *payload, size_t payload_len,
                           QuicCipherKeys *keys);

/* 解析长头包（含移除 Header Protection + 解密）。
 * 成功返回 0，失败（解密错误、格式错误）返回 <0。 */
int quic_packet_parse_long(const uint8_t *data, size_t len,
                           int *pkt_type,
                           QuicConnectionId *src_cid,
                           QuicConnectionId *dst_cid,
                           const uint8_t **token, size_t *token_len,
                           uint64_t *pn,
                           const uint8_t **payload, size_t *payload_len,
                           QuicCipherKeys *keys,
                           uint64_t expected_pn,
                           size_t *consumed);

/* Retry 包 (RFC 9000 §17.2.5) — 服务端地址验证 */
int  quic_packet_build_retry(uint8_t *out, size_t *out_len,
                              const QuicConnectionId *original_dcid,
                              const QuicConnectionId *scid,
                              const uint8_t *token, size_t token_len);

/* ============================================
 * 短头包 (1-RTT) — RFC 9000 §17.3
 * ============================================ */

int quic_packet_build_short(uint8_t *out, size_t *out_len,
                            const QuicConnectionId *dst_cid,
                            uint64_t pn,
                            const uint8_t *payload, size_t payload_len,
                            QuicCipherKeys *keys,
                            int key_phase,
                            int spin_bit);

int quic_packet_parse_short(const uint8_t *data, size_t len,
                            QuicConnectionId *dst_cid,
                            uint64_t *pn,
                            const uint8_t **payload, size_t *payload_len,
                            QuicCipherKeys *keys,
                            uint64_t expected_pn);

/* ============================================
 * 帧编解码 (RFC 9000 §19)
 * ============================================ */

/* CRYPTO frame (§19.6) */
int quic_frame_write_crypto(uint8_t *buf, size_t cap,
                            uint64_t offset,
                            const uint8_t *data, size_t len);
int quic_frame_parse_crypto(const uint8_t *data, size_t len,
                            uint64_t *offset,
                            const uint8_t **crypto_data, size_t *crypto_len);

/* ACK frame (§19.3) — 只输出 ACK (0x02)，不含 ECN */
int quic_frame_write_ack(uint8_t *buf, size_t cap,
                         const QuicAckFrame *ack);
int quic_frame_parse_ack(const uint8_t *data, size_t len,
                         QuicAckFrame *ack);

/* CONNECTION_CLOSE frame (§19.19) */
int quic_frame_write_connection_close(uint8_t *buf, size_t cap,
                                      uint64_t error_code,
                                      const char *reason, size_t reason_len);
int quic_frame_parse_connection_close(const uint8_t *data, size_t len,
                                      uint64_t *error_code,
                                      const char **reason, size_t *reason_len);

/* PADDING frame (§19.1) — count 个 0x00 字节 */
int quic_frame_write_padding(uint8_t *buf, size_t cap, size_t count);

/* PING frame (§19.2) — 单个 0x01 */
int quic_frame_write_ping(uint8_t *buf, size_t cap);

/* STREAM frame (§19.8) — type 0x08..0x0f */
int quic_frame_write_stream(uint8_t *buf, size_t cap,
                            uint64_t stream_id, uint64_t offset, int fin,
                            const uint8_t *data, size_t len);
int quic_frame_parse_stream(const uint8_t *data, size_t len,
                            uint64_t *stream_id, uint64_t *offset, int *fin,
                            const uint8_t **stream_data, size_t *stream_len);

/* MAX_DATA frame (§19.9) */
int quic_frame_write_max_data(uint8_t *buf, size_t cap, uint64_t max_data);
int quic_frame_parse_max_data(const uint8_t *data, size_t len, uint64_t *max_data);

/* MAX_STREAM_DATA frame (§19.10) */
int quic_frame_write_max_stream_data(uint8_t *buf, size_t cap,
                                     uint64_t stream_id, uint64_t max_data);
int quic_frame_parse_max_stream_data(const uint8_t *data, size_t len,
                                     uint64_t *stream_id, uint64_t *max_data);

/* MAX_STREAMS frame (§19.11) — bidi=1 for BIDI, bidi=0 for UNI */
int quic_frame_write_max_streams(uint8_t *buf, size_t cap,
                                  uint64_t max_streams, int bidi);
int quic_frame_parse_max_streams(const uint8_t *data, size_t len,
                                  uint64_t *max_streams, int *bidi);

/* RESET_STREAM frame (§19.4) */
int quic_frame_write_reset_stream(uint8_t *buf, size_t cap,
                                  uint64_t stream_id, uint64_t error_code,
                                  uint64_t final_size);
int quic_frame_parse_reset_stream(const uint8_t *data, size_t len,
                                  uint64_t *stream_id, uint64_t *error_code,
                                  uint64_t *final_size);

/* STOP_SENDING frame (§19.5) */
int quic_frame_write_stop_sending(uint8_t *buf, size_t cap,
                                  uint64_t stream_id, uint64_t error_code);
int quic_frame_parse_stop_sending(const uint8_t *data, size_t len,
                                  uint64_t *stream_id, uint64_t *error_code);

/* NEW_CONNECTION_ID frame (§19.15) — 返回消耗的字节数 */
int quic_frame_parse_new_conn_id(const uint8_t *data, size_t len,
                                  uint64_t *seq, uint64_t *retire_prior_to,
                                  uint8_t *cid_len, const uint8_t **cid_data,
                                  const uint8_t **reset_token);

/* NEW_TOKEN frame (§19.7) — 返回消耗的字节数 */
int quic_frame_parse_new_token(const uint8_t *data, size_t len);

/* RETIRE_CONNECTION_ID frame (§19.16) */
int quic_frame_write_retire_conn_id(uint8_t *buf, size_t cap,
                                     uint64_t seq_num);
int quic_frame_parse_retire_conn_id(const uint8_t *data, size_t len,
                                     uint64_t *seq_num);

/* ── BLOCKED frames (RFC 9000 §19.14–19.17) ────────────── */

int quic_frame_write_data_blocked(uint8_t *buf, size_t cap,
                                   uint64_t max_data);
int quic_frame_parse_data_blocked(const uint8_t *data, size_t len,
                                   uint64_t *max_data);

int quic_frame_write_stream_data_blocked(uint8_t *buf, size_t cap,
                                          uint64_t stream_id,
                                          uint64_t max_stream_data);
int quic_frame_parse_stream_data_blocked(const uint8_t *data, size_t len,
                                          uint64_t *stream_id,
                                          uint64_t *max_stream_data);

int quic_frame_write_streams_blocked(uint8_t *buf, size_t cap,
                                      uint64_t max_streams, int bidi);
int quic_frame_parse_streams_blocked(const uint8_t *data, size_t len,
                                      uint64_t *max_streams, int *bidi);

/* ── DATAGRAM frames (RFC 9221 §4) ────────── */
int quic_frame_write_datagram(uint8_t *buf, size_t cap,
                               const uint8_t *data, size_t len);
int quic_frame_parse_datagram(const uint8_t *data, size_t len,
                               const uint8_t **payload, size_t *payload_len);

#ifdef __cplusplus
}
#endif

#endif /* QUIC_PACKET_H */
