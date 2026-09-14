#ifndef HTTP3_COMMON_H
#define HTTP3_COMMON_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================
 * HTTP/3 帧类型 (RFC 9114 §7.2)
 * ============================================ */

#define H3_FRAME_DATA           0x00
#define H3_FRAME_HEADERS        0x01
#define H3_FRAME_SETTINGS       0x04
#define H3_FRAME_GOAWAY         0x07

/* ── SETTINGS parameters ── */
#define H3_SETTING_QPACK_MAX_TABLE_CAPACITY  0x01
#define H3_SETTING_MAX_FIELD_SECTION_SIZE    0x06
#define H3_SETTING_ENABLE_CONNECT_PROTOCOL     0x08        /* Extended CONNECT, RFC 9220 */
#define H3_SETTING_ENABLE_WEBTRANSPORT_DRAFT02  0x2b603742  /* draft-02 ~ draft-06 */
#define H3_SETTING_ENABLE_WEBTRANSPORT          0x2c7cf000  /* RFC 9220 最终值 */
#define H3_SETTING_H3_DATAGRAM               0x33        /* RFC 9297 */

/* ============================================
 * HTTP/3 Stream 约定 (RFC 9114 §6)
 * ============================================ */

#define H3_CONTROL_STREAM_ID    0x00   /* 客户端→服务端控制流 */
#define H3_QPACK_ENC_STREAM_ID  0x02   /* 客户端→服务端 QPACK encoder */
#define H3_QPACK_DEC_STREAM_ID  0x03   /* 客户端→服务端 QPACK decoder */
#define H3_SERVER_CONTROL_ID    0x00   /* 服务端→客户端控制流 (uni) */

/* UNI stream type (RFC 9114 §6.2) */
#define H3_STREAM_TYPE_CONTROL      0x00
#define H3_STREAM_TYPE_PUSH         0x01
#define H3_STREAM_TYPE_QPACK_ENC    0x02
#define H3_STREAM_TYPE_QPACK_DEC    0x03
#define H3_STREAM_TYPE_WEBTRANSPORT 0x54  /* draft-ietf-webtrans-http3 */

/* 服务端 unicast stream ID（客户端侧: H3_SERVER_CONTROL_ID=0，
 * 实际 QUIC stream_id 由 QuicConnectionStreamOpenUni 分配） */

/* ============================================
 * 简易 header 编码（纯文本，暂不实现 QPACK）
 *
 * 请求格式:  ":method GET\r\n:path /index.html\r\n\r\n"
 * 响应格式:  "200 OK\r\ncontent-length: 12\r\n\r\nHello World!"
 * ============================================ */

/* 解析 :method / :path 开头的简单请求行 */
int h3_parse_request_headers(const uint8_t *data, size_t len,
                              const char **method, const char **path);

/* 构造简易响应头部 */
int h3_format_response_headers(uint8_t *buf, size_t cap,
                                int status, const char *status_text,
                                const char *extra_headers);

/* ============================================
 * Quic Varint (从 quic_packet.c 复用)
 * ============================================ */

int      quic_varint_encode(uint8_t *buf, size_t cap, uint64_t val);
int      quic_varint_decode(const uint8_t *data, size_t len, uint64_t *val);
size_t   quic_varint_len(uint64_t val);

#ifdef __cplusplus
}
#endif

#endif /* HTTP3_COMMON_H */
