#ifndef QUIC_LISTENER_H
#define QUIC_LISTENER_H

#include "quic_common.h"
#include <uv.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================
 * QuicListener — 服务端监听器
 * ============================================ */

typedef struct QuicListener QuicListener;

QuicListener* QuicListenerCreate(uv_loop_t *loop,
                                 const char *cert_file, const char *key_file);
void          QuicListenerDestruct(QuicListener *l);

void QuicListenerSetOnConnection(QuicListener *l,
                                 QuicListenerOnConnection cb);

void QuicListenerSetAppData(QuicListener *l, void *data);
void* QuicListenerGetAppData(QuicListener *l);

int  QuicListenerListen(QuicListener *l, const char *ip, int port);

/* 启用 Retry 地址验证 (RFC 9000 §8.1.2) — 客户端首次连接时先发 Retry */
void QuicListenerSetRetryEnabled(QuicListener *l, int enabled);

/* 上层（如 http3_server）使用此函数在连接关闭时从监听器列表清理 */
void QuicListenerRemoveConn(QuicListener *l, QuicConnection *conn);

#ifdef __cplusplus
}
#endif

#endif /* QUIC_LISTENER_H */
