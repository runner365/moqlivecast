#ifndef TLS_CLIENT_H
#define TLS_CLIENT_H
#include <uv.h>
#include <openssl/ssl.h>
#include <stdint.h>
#include "tls_common.h"

typedef struct TlsClient TlsClient;

typedef void (*TlsClientOnHandshakeDone)(TlsClient *c, int status);
typedef void (*TlsClientOnSecret)(TlsClient *c, uint32_t level, int direction,
                                   const unsigned char *secret, size_t len);
typedef void (*TlsClientOnClose)(TlsClient *c);

struct TlsClient {
    uv_loop_t  *loop_;
    SSL_CTX    *ssl_ctx_;
    SSL        *ssl_;
    struct UdpClient *udp_;

    /* 按加密级别分别缓冲接收数据 */
    unsigned char *recv_buf_[TLS_LEVEL_NUM];
    size_t         recv_buf_len_[TLS_LEVEL_NUM];
    uint32_t       renc_level_;   /* 当前读级别 */

    /* 服务端地址 */
    char server_ip_[64];
    int  server_port_;

    int handshake_done_;

    TlsClientOnHandshakeDone on_handshake_done_;
    TlsClientOnSecret        on_secret_;
    TlsClientOnClose         on_close_;
};

TlsClient* TlsClientConstruct(uv_loop_t *loop);
void       TlsClientDestruct(TlsClient *c);

void TlsClientSetOnHandshakeDone(TlsClient *c, TlsClientOnHandshakeDone cb);
void TlsClientSetOnSecret(TlsClient *c, TlsClientOnSecret cb);
void TlsClientSetOnClose(TlsClient *c, TlsClientOnClose cb);

int TlsClientConnect(TlsClient *c, const char *ip, int port);

/* 内部使用：UDP 收到数据后调用，驱动握手 */
void TlsClientFeedData(TlsClient *c, const char *data, size_t len);

#endif
