#ifndef TLS_SERVER_H
#define TLS_SERVER_H
#include <uv.h>
#include <openssl/ssl.h>
#include <stdint.h>
#include "tls_common.h"

typedef struct TlsServer TlsServer;

typedef void (*TlsServerOnHandshakeDone)(TlsServer *s, int status);
typedef void (*TlsServerOnSecret)(TlsServer *s, uint32_t level, int direction,
                                   const unsigned char *secret, size_t len);
typedef void (*TlsServerOnClose)(TlsServer *s);

struct TlsServer {
    uv_loop_t  *loop_;
    SSL_CTX    *ssl_ctx_;
    SSL        *ssl_;
    struct UdpServer *udp_;

    unsigned char *recv_buf_[TLS_LEVEL_NUM];
    size_t         recv_buf_len_[TLS_LEVEL_NUM];
    uint32_t       renc_level_;

    char client_ip_[64];
    int  client_port_;

    int handshake_done_;

    TlsServerOnHandshakeDone on_handshake_done_;
    TlsServerOnSecret        on_secret_;
    TlsServerOnClose         on_close_;
};

TlsServer* TlsServerConstruct(uv_loop_t *loop,
                               const char *cert_file, const char *key_file);
void       TlsServerDestruct(TlsServer *s);

void TlsServerSetOnHandshakeDone(TlsServer *s, TlsServerOnHandshakeDone cb);
void TlsServerSetOnSecret(TlsServer *s, TlsServerOnSecret cb);
void TlsServerSetOnClose(TlsServer *s, TlsServerOnClose cb);

int TlsServerListen(TlsServer *s, const char *ip, int port);

#endif
