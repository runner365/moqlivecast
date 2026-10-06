#ifndef HTTP3_SERVER_H
#define HTTP3_SERVER_H

#include "quic_listener.h"
#include "quic_connection.h"
#include "http3_common.h"
#include <uv.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================
 * HTTP/3 Server
 * ============================================ */

typedef struct http3_server http3_server;

/* 回调: 收到完整请求（HEADERS + DATA 结束）后调用 */
typedef void (*http3_on_request)(http3_server *srv,
                                  QuicConnection *conn,
                                  uint64_t stream_id,
                                  const uint8_t *hdrs, size_t hdrs_len,
                                  const uint8_t *body, size_t body_len);

http3_server *http3_server_create(uv_loop_t *loop,
                                   const char *cert_file,
                                   const char *key_file,
                                   http3_on_request on_req);

void          http3_server_destroy(http3_server *srv);

int           http3_server_listen(http3_server *srv,
                                   const char *ip, int port);

/* 本端口接受的应用协议（ALPN），服务端偏好优先。
 * **必须在 http3_server_listen 之前调用** —— ALPN 在握手时协商，之后就晚了。
 * protos == NULL / count <= 0 → 保持默认（h3 + quic-echo）。
 *
 * 同一个端口上跑多个协议就是靠这张表：包按连接 ID 分，握手时用 ALPN 定一次
 * 这条连接归谁。返回 0 成功，<0 失败。 */
int           http3_server_set_alpn(http3_server *srv,
                                     const char *const *protos, int count);

/* 设置 / 获取 应用层数据（供 API wrapper 使用） */
void  http3_server_set_user_data(http3_server *srv, void *data);
void* http3_server_get_user_data(http3_server *srv);

/* 非 h3 的 ALPN 连接回调。
 *
 * 同一个 QUIC 端口可以按 ALPN 承载多个应用协议（h3 → WebTransport；
 * moq-lite-06 → 裸 QUIC 的 moq-lite）。分派时刻是**握手完成**，不是收到首个
 * Initial 包的时候 —— ALPN 要到握手才定得下来，所以 on_connection 太早。
 *
 * 注册方在回调里自行接线（QuicConnectionSetOnStreamData / SetOnClose / ...），
 * h3 那套（SETTINGS、QPACK 流）不会再给对方发。未注册时这类连接直接关闭。
 *
 * ⚠️ 交割之后这条连接**整条归注册方**：http3_server 已经把它从 H3 连接表摘掉、
 * 清掉 datagram/on_connected 回调。因此连 listener 记账也转交出去了 ——
 * 注册方在 on_close 里要自己调
 *     QuicListenerRemoveConn(QuicConnectionGetListener(conn), conn);
 * 否则连接关闭后 listener 的连接表会一直留着它。 */
typedef void (*http3_on_other_alpn_fn)(void *user_data, QuicConnection *conn,
                                        const char *alpn,
                                        const char *remote_ip, int remote_port);
void http3_server_set_on_other_alpn(http3_server *srv,
                                     http3_on_other_alpn_fn cb,
                                     void *user_data);

/* DATAGRAM 回调 — API wrapper 注册，http3_server 收到后转发 */
typedef void (*http3_server_on_dgram_fn)(void *user_data, void *conn,
    const uint8_t *data, size_t len);
void http3_server_set_on_datagram(http3_server *srv,
                                   http3_server_on_dgram_fn cb);

/* 发送响应 (HEADERS + DATA) */
int  http3_send_response(QuicConnection *conn, uint64_t stream_id,
                          int status, const char *status_text,
                          const char *content_type,
                          const uint8_t *body, size_t body_len);

#ifdef __cplusplus
}
#endif

#endif /* HTTP3_SERVER_H */
