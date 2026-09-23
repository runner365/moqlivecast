#ifndef WEBTRANSPORT_CLIENT_API_H
#define WEBTRANSPORT_CLIENT_API_H

#include <stdint.h>
#include <stddef.h>
#include <uv.h>
#include "quic_connection.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================
 * WebTransport 客户端 API
 *
 * 所有函数任意线程可调用，线程安全。
 * 所有回调在内部 loop 线程中执行。
 * ============================================ */

typedef struct wt_client     wt_client_t;
typedef struct wt_stream     wt_stream_t;

/* ── 回调类型 ─────────────────────────────── */

/* status: 0=成功, 非0=失败 */
typedef void (*wt_on_connect_fn)(wt_client_t *cli, int status, void *user);

/* stream 创建成功，可以开始写数据 */
typedef void (*wt_on_stream_open_fn)(wt_client_t *cli, wt_stream_t *s, void *user);

/* 收到某 stream 上的数据 */
typedef void (*wt_on_stream_data_fn)(wt_client_t *cli, wt_stream_t *s,
                                     const uint8_t *data, size_t len, void *user);

/* stream 被对端关闭 */
typedef void (*wt_on_stream_close_fn)(wt_client_t *cli, wt_stream_t *s, void *user);

/* wt 会话关闭 */
typedef void (*wt_on_close_fn)(wt_client_t *cli, int err, void *user);

typedef struct {
    wt_on_connect_fn      on_connect;       /* 握手完成 */
    wt_on_stream_open_fn  on_stream_open;   /* stream 已就绪，可写数据 */
    wt_on_stream_data_fn  on_stream_data;   /* 收到 stream 数据 */
    wt_on_stream_close_fn on_stream_close;  /* stream 关闭 */
    wt_on_close_fn        on_close;          /* 会话关闭 */
    void                 *user_data;
} wt_callbacks_t;

/* ── 生命周期 ─────────────────────────────── */

/* 创建客户端。返回 NULL → OOM */
wt_client_t *wt_client_new(uv_loop_t *loop, wt_callbacks_t *cb);

/* 异步连接（任意线程调用）。握手+SETTINGS+CONNECT 全自动。
 * 结果 → cb.on_connect(status) */
void wt_client_connect(wt_client_t *cli, const char *host, int port);

/* 同上，但可指定 HTTP/3 的 :path（含 query，如
 * "/moq?app=live&stream=123456"）。:authority 由 host:port 自动生成。
 * wt_client_connect() 等价于 path="/"。 */
void wt_client_connect_path(wt_client_t *cli, const char *host, int port,
                            const char *path);

/* 关闭客户端 */
void wt_client_close(wt_client_t *cli);

/* ── 回调上下文 ────────────────────────────── */

/* 获取 user_data */
void *wt_client_get_user_data(wt_client_t *cli);

/* 底层 QUIC 路径 / 拥塞快照。成功返回 0；未建连返回 -1 */
int wt_client_get_quic_stats(wt_client_t *cli, QuicConnectionStats *out);

/* ── Stream ────────────────────────────────── */

/* 打开一个双向 stream（任意线程调用）。
 * 初始化完成后通过 cb.on_stream_data 接收数据。 */
void wt_client_open_stream(wt_client_t *cli);

/* 打开一个单向 stream（任意线程调用）。
 * 语义：本端只写，对端只读；不会收到该流上的 on_stream_data。
 * 通过 cb.on_stream_open 拿到 wt_stream_t，再用
 * wt_stream_is_uni() 区分它与双向流。
 * MOQ 规范要求对象走单向流，控制流也是一对单向流。 */
void wt_client_open_uni_stream(wt_client_t *cli);

/* 该流是否为单向流（0 = 双向）。 */
int wt_stream_is_uni(wt_stream_t *s);

/* 写数据回调：ret=0 全部 ACK，ret=-2 超时，ret<0 错误。
 * 回调在 loop 线程中调用，可安全再次调用 wt_stream_write_cb 继续发送。
 * timeout_ms=0 默认 5000ms。 */
typedef void (*wt_stream_write_cb_fn)(wt_stream_t *s, int ret, void *user);
void wt_stream_write_cb(wt_stream_t *s, const uint8_t *data, size_t len,
                         wt_stream_write_cb_fn cb, void *user,
                         uint64_t timeout_ms);

/* 关闭 stream */
void wt_stream_close(wt_stream_t *s);

#ifdef __cplusplus
}
#endif

#endif /* WEBTRANSPORT_CLIENT_API_H */
