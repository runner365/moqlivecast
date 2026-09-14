#ifndef QUIC_CLIENT_API_H
#define QUIC_CLIENT_API_H

#include <stdint.h>
#include <stddef.h>
#include <uv.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================
 * QUIC 客户端 API — 线程安全，callback 驱动
 *
 * 模式参考 uv_tcp_t: async connect, async write,
 * 所有回调在 loop 线程中执行。
 * ============================================ */

typedef struct quic_client quic_client_t;
typedef struct quic_stream  quic_stream_t;

/* ── 回调类型 ─────────────────────────────── */

/* connect: status=0 成功, <0 失败 */
typedef void (*quic_client_on_connect_cb)(quic_client_t *c, int status, void *user);

/* stream 打开成功，开始写数据 */
typedef void (*quic_client_on_stream_cb)(quic_client_t *c, quic_stream_t *s, void *user);

/* 收到对端 stream 数据 */
typedef void (*quic_client_on_data_cb)(quic_client_t *c, quic_stream_t *s,
                                        const uint8_t *data, size_t len, void *user);

/* 连接关闭 */
typedef void (*quic_client_on_close_cb)(quic_client_t *c, int err, void *user);

/* write callback: ret=0 全部 ACK, ret=-2 超时, ret<0 错误。
 * 回调在 loop 线程中执行，可安全再次调用 quic_client_stream_write 继续发送 */
typedef void (*quic_client_write_cb)(quic_stream_t *s, int ret, void *user);

typedef struct {
    quic_client_on_connect_cb on_connect;
    quic_client_on_stream_cb  on_stream_open;
    quic_client_on_data_cb    on_stream_data;
    quic_client_on_close_cb   on_close;
    void                     *user_data;
} quic_client_callbacks_t;

/* ── 生命周期 ─────────────────────────────── */

/* 创建客户端。返回 NULL → OOM */
quic_client_t *quic_client_new(uv_loop_t *loop, quic_client_callbacks_t *cb);

/* 异步连接（任意线程调用）。结果 → cb.on_connect(status) */
void quic_client_connect(quic_client_t *c, const char *ip, int port);

/* 关闭客户端 */
void quic_client_close(quic_client_t *c);

/* ── Stream ────────────────────────────────── */

/* 打开双向 stream（任意线程调用）。就绪后 cb.on_stream_open */
void quic_client_open_stream(quic_client_t *c);

/* 写数据（任意线程调用）。timeout_ms=0 默认 30000ms。
 * 全部 ACK → cb(s, 0, user)。超时 → cb(s, -2, user)。错误 → cb(s, err, user)。
 * fin=1 标记流结束，对端收到 FIN 后不会再收到数据 */
void quic_client_stream_write(quic_stream_t *s,
                               const uint8_t *data, size_t len, int fin,
                               quic_client_write_cb cb, void *user,
                               uint64_t timeout_ms);

/* 关闭流发送端（发送 FIN） */
void quic_client_stream_close(quic_stream_t *s);

#ifdef __cplusplus
}
#endif

#endif /* QUIC_CLIENT_API_H */
