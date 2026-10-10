#ifndef QUIC_CLIENT_API_H
#define QUIC_CLIENT_API_H

#include <stdint.h>
#include <stddef.h>
#include <uv.h>
#include "quic_common.h"   /* QuicConnection（quic_client_conn 的返回类型） */

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

/* 同上的单向（uni）版本。签名与 bidi 的**故意保持一致** —— 两者的处置完全
 * 一样（拿 s 去写），分开只是为了让实现知道该调哪个 QuicConnection 开启函数。
 * 与 bidi 不同：uni 流收不到对端数据，写完关门即可。 */
typedef void (*quic_client_on_stream_open_uni_cb)(quic_client_t *c,
                                                 quic_stream_t *s, void *user);

/* 收到对端 stream 数据 */
typedef void (*quic_client_on_data_cb)(quic_client_t *c, quic_stream_t *s,
                                        const uint8_t *data, size_t len, void *user);

/* 连接关闭 */
typedef void (*quic_client_on_close_cb)(quic_client_t *c, int err, void *user);

/* 对端 RESET 了某条 stream（error_code = 对端的应用错误码）。
 * 在上层协议拿 RESET 当「拒绝 / 放弃」用的场合是必需信号 —— 见
 * QuicConnectionSetOnStreamReset 的说明。 */
typedef void (*quic_client_on_stream_reset_cb)(quic_client_t *c,
                                               quic_stream_t *s,
                                               uint64_t error_code, void *user);

/* write callback: ret=0 全部 ACK, ret=-2 超时, ret<0 错误。
 * 回调在 loop 线程中执行，可安全再次调用 quic_client_stream_write 继续发送 */
typedef void (*quic_client_write_cb)(quic_stream_t *s, int ret, void *user);

typedef struct {
    quic_client_on_connect_cb on_connect;
    quic_client_on_stream_cb  on_stream_open;
    quic_client_on_data_cb    on_stream_data;
    quic_client_on_close_cb   on_close;
    void                     *user_data;
    /* 追加在末尾：既有的具名初始化/整体 memset 不受影响。
     * 不开 uni 流（只 quic_client_open_stream）的调用方可以一直是 NULL。 */
    quic_client_on_stream_open_uni_cb on_stream_open_uni;
    /* 同样追加在末尾，可为 NULL。s 可能是 NULL：对端可以 reset 一条我们
     * 只发过、没收过数据的流（那种流在客户端本地还没建记录）—— 照样通知，
     * 因为「它被拒了」这个事实不依赖我们有没有那条流的记录。 */
    quic_client_on_stream_reset_cb    on_stream_reset;
} quic_client_callbacks_t;

/* ── 生命周期 ─────────────────────────────── */

/* 创建客户端。返回 NULL → OOM */
quic_client_t *quic_client_new(uv_loop_t *loop, quic_client_callbacks_t *cb);

/* 异步连接（任意线程调用）。结果 → cb.on_connect(status) */
void quic_client_connect(quic_client_t *c, const char *ip, int port);

/* 本客户端 offer 的 ALPN 列表（任意线程调用，**须在 quic_client_connect 之前**）。
 *
 * 不调则用 tls_ctx_set_alpn 的写死默认 —— (h3 + quic-echo)。裸 QUIC 的
 * moq-lite 客户端必须显式设成 {"moq-lite-06"}：
 *   - 服务端只 offer moq-lite-06、客户端不带它 → 直接 alert no_application_protocol；
 *   - 两边都 offer h3 时协商会落到 h3 → 服务端按 WebTransport 处理，等一个
 *     永远不来的 CONNECT。**症状是"连上了但没有任何下文"**，很难查。
 *
 * protos 为 C 字符串数组，顺序即 offer 顺序（靠前的优先）。内部做**深拷贝**，
 * 调用方不必保持字符串存活。count<=0 或全为空串 = 恢复默认。
 * 再次调用为整表替换。 */
void quic_client_set_alpn(quic_client_t *c, const char *const *protos,
                           int count);

/* 关闭客户端 */
void quic_client_close(quic_client_t *c);

/* ── Stream ────────────────────────────────── */

/* 打开双向 stream（任意线程调用）。就绪后 cb.on_stream_open */
void quic_client_open_stream(quic_client_t *c);

/* 打开单向 stream（任意线程调用）。就绪后 cb.on_stream_open_uni。
 * 开流请求按**调用顺序**完成，所以混着开 bidi/uni 时两个回调的先后也依此 ——
 * 调用方可以据此把「第 n 次开流」和回调对上（moq-lite 的 Setup 流就靠这个
 * 关联：开的时候还不知道 stream id）。 */
void quic_client_open_uni_stream(quic_client_t *c);

/* 写数据（任意线程调用）。timeout_ms=0 默认 30000ms。
 * 全部 ACK → cb(s, 0, user)。超时 → cb(s, -2, user)。错误 → cb(s, err, user)。
 * fin=1 标记流结束，对端收到 FIN 后不会再收到数据 */
void quic_client_stream_write(quic_stream_t *s,
                               const uint8_t *data, size_t len, int fin,
                               quic_client_write_cb cb, void *user,
                               uint64_t timeout_ms);

/* 关闭流发送端（发送 FIN） */
void quic_client_stream_close(quic_stream_t *s);

/* 复位 stream（RESET_STREAM）。对端那条读方向会收到流错误 —— moq-lite 里
 * 「拒绝这条请求」就是这么做（没有拒绝消息，reset 即拒绝）。 */
void quic_client_stream_reset(quic_stream_t *s, uint64_t error_code);

/* 底层 QuicConnection（NULL = 还没连上）。给需要 conn 级能力的上层用
 * （例如在开流之前设 ALPN / 取 GetAlpn）。生命周期与 client 绑定。 */
QuicConnection *quic_client_conn(quic_client_t *c);

/* 这条流是不是 QUIC 单向流（QUIC 流 id 的 0x2 位，RFC 9000 §2.1）。
 * 上层应用协议常常要按这个分命名空间（moq-lite：单向的是 Group/Setup，
 * 双向的是请求/应答），而 quic_stream_t 是不透明类型，所以在这里透出来。 */
int quic_client_stream_is_uni(const quic_stream_t *s);

#ifdef __cplusplus
}
#endif

#endif /* QUIC_CLIENT_API_H */
