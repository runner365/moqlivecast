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

/* 设置 / 获取 应用层数据（供 API wrapper 使用） */
void  http3_server_set_user_data(http3_server *srv, void *data);
void* http3_server_get_user_data(http3_server *srv);

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
