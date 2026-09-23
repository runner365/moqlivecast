#ifndef WEBTRANSPORT_SERVER_API_H
#define WEBTRANSPORT_SERVER_API_H

#include <stdint.h>
#include <stddef.h>
#include <uv.h>
#include "http3_server_api.h"
#include "quic_connection.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct wt_server   wt_server_t;
typedef struct wt_session  wt_session_t;
typedef struct wt_stream   wt_stream_t;

/* ============================================
 * Session info (path + query params)
 * ============================================ */
const char *wt_session_get_path(wt_session_t *sess);
const char *wt_session_get_param(wt_session_t *sess, const char *name);
int         wt_session_param_count(wt_session_t *sess);
int         wt_session_param_at(wt_session_t *sess, int i,
                                const char **name, const char **value);

/* ============================================
 * 回调类型
 * ============================================ */

/* path 级别的 session 回调 */
typedef void (*wt_srv_on_session_fn)(wt_server_t *srv, wt_session_t *sess,
                                      void *user);

/* stream 收到数据（任意线程调用 wt_stream_write 发送） */
typedef void (*wt_srv_on_stream_data_fn)(wt_server_t *srv, wt_session_t *sess,
                                          wt_stream_t *st,
                                          const uint8_t *data, size_t len,
                                          void *user);

/* session 关闭 */
typedef void (*wt_srv_on_session_close_fn)(wt_server_t *srv, wt_session_t *sess,
                                            void *user);

/* 每个路由的回调集合 */
typedef struct {
    wt_srv_on_session_fn       on_session;
    wt_srv_on_stream_data_fn   on_stream_data;
    wt_srv_on_session_close_fn on_session_close;
    void                      *user_data;
} wt_path_callbacks_t;

/* ============================================
 * 服务端 API
 * ============================================ */

/* 创建服务端 (loop 线程调用) */
wt_server_t *wt_server_create(uv_loop_t *loop);

/* 绑定 + TLS + 启动监听 */
int wt_server_listen(wt_server_t *srv,
                      const char *cert_file, const char *key_file,
                      const char *ip, int port);

/* 注册 path 路由（支持多次调用） */
void wt_server_add_path(wt_server_t *srv, const char *path,
                         wt_path_callbacks_t *cb);

/* 注册普通 HTTP/3 路由（非 WebTransport），如 POST /keepalive。
 * 透传到底层 http3_server_api_add_handler。 */
int  wt_server_add_http_handler(wt_server_t *srv,
                                 http3_method       method,
                                 const char        *path,
                                 http3_handler_fn   callback);

/* 销毁 */
void wt_server_destroy(wt_server_t *srv);

/* ============================================
 * Session API
 * ============================================ */

void  wt_session_set_user_data(wt_session_t *sess, void *data);
void *wt_session_get_user_data(wt_session_t *sess);

uint64_t wt_session_get_id(wt_session_t *sess);

/* 主动关闭 session（关闭底层 QUIC 连接） */
void wt_session_close(wt_session_t *sess);

/* 底层 QUIC 路径 / 拥塞快照。成功返回 0 */
int wt_session_get_quic_stats(wt_session_t *sess, QuicConnectionStats *out);

/* 主动打开 stream。回调在 stream 就绪时调用 */
typedef void (*wt_session_on_stream_open_fn)(wt_server_t *srv,
                                              wt_session_t *sess,
                                              wt_stream_t *st, void *user);
void wt_session_open_stream(wt_session_t *sess,
                             wt_session_on_stream_open_fn cb, void *user);

/* ============================================
 * Stream API
 * ============================================ */

void  wt_stream_set_user_data(wt_stream_t *st, void *data);
void *wt_stream_get_user_data(wt_stream_t *st);
wt_session_t *wt_stream_get_session(wt_stream_t *st);

/* 该流是否为单向流。
 * 单向流的语义是「只有发起方可以写」：对端（本端）只能读，回写会触发
 * 对端 PROTOCOL_VIOLATION。wt_stream_write 已内建拒绝，此处供上层
 * 在路由/记账时区分，不必依赖 write 失败来发现。 */
int   wt_stream_is_uni(wt_stream_t *st);

/* 服务端主动开一条单向流（MOQ 规范要求对象走单向流）。
 *
 * 返回新建的 wt_stream_t（用于 wt_stream_write 发送），失败返回 NULL。
 * 失败通常是客户端尚未给出 MAX_STREAMS_UNI 配额 —— 这是流控的正常
 * 情形，调用方应稍后重试，而不是当作致命错误。
 *
 * 该流由本端发起：可写、不会收到 on_stream_data（对端不能回写）。
 * 用毕调 wt_stream_close() 发 FIN。 */
wt_stream_t *wt_server_open_uni_stream(wt_session_t *sess);

/* 写数据（自动加 WT 帧头）
 * 返回：0 已接受；1 拥塞（未接受，调用方应缓存）；-1 异常
 * 注意：对端发起的单向流上调用会被拒绝（返回 -1）。 */
int  wt_stream_write(wt_stream_t *st, const uint8_t *data, size_t len);
void wt_stream_close(wt_stream_t *st);

/* send_buf 降到低水位时回调（QUIC loop 线程） */
typedef void (*wt_stream_on_writable_fn)(wt_stream_t *st, void *user);
void wt_stream_set_on_writable(wt_stream_t *st, wt_stream_on_writable_fn cb, void *user);

#ifdef __cplusplus
}
#endif

#endif /* WEBTRANSPORT_SERVER_API_H */
