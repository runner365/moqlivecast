#include "tls_client.h"
#include "tls_common.h"
#include "udp_client.h"
#include "logger.h"
#include <openssl/core_dispatch.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ============================================
 * 内部 — QUIC TLS dispatch 回调
 * ============================================ */

static int crypto_send_cb(SSL *s, const unsigned char *buf, size_t buf_len,
                          size_t *consumed, void *arg) {
    TlsClient *c = (TlsClient*)arg;
    (void)s;

    int ret = UdpClientSend(c->udp_, (const char*)buf, buf_len,
                            c->server_ip_, c->server_port_);
    if (ret < 0) {
        LOG_ERROR("[tls-client] crypto_send error: %s", uv_strerror(ret));
        return 0;
    }
    *consumed = buf_len;
    LOG_DEBUG("[tls-client] crypto_send: %zu bytes", buf_len);
    return 1;
}

static int crypto_recv_rcd_cb(SSL *s, const unsigned char **buf,
                              size_t *bytes_read, void *arg) {
    TlsClient *c = (TlsClient*)arg;
    (void)s;

    int level = (int)c->renc_level_;
    /*
     * 关键：QUIC CRYPTO frame 流可能分片到达。
     * 至少需要 5 字节 TLS 记录头 + 16 字节 GCM auth tag = 21 字节。
     * 不足时返回 0，让 OpenSSL 继续等待。
     */
    if (c->recv_buf_len_[level] < 21) {
        *buf        = NULL;
        *bytes_read = 0;
        LOG_DEBUG("[tls-client] crypto_recv_rcd: level=%s, %zu bytes (waiting for more)",
               tls_prot_level_name((uint32_t)level), c->recv_buf_len_[level]);
        return 1;
    }

    *buf        = c->recv_buf_[level];
    *bytes_read = c->recv_buf_len_[level];
    LOG_DEBUG("[tls-client] crypto_recv_rcd: level=%s, returning %zu bytes",
           tls_prot_level_name((uint32_t)level), *bytes_read);
    return 1;
}

static int crypto_release_rcd_cb(SSL *s, size_t bytes_read, void *arg) {
    TlsClient *c = (TlsClient*)arg;
    (void)s;

    int level = (int)c->renc_level_;
    if (bytes_read > 0 && bytes_read <= c->recv_buf_len_[level]) {
        size_t remaining = c->recv_buf_len_[level] - bytes_read;
        if (remaining > 0) {
            memmove(c->recv_buf_[level],
                    c->recv_buf_[level] + bytes_read, remaining);
        }
        c->recv_buf_len_[level] = remaining;
    }
    LOG_DEBUG("[tls-client] crypto_release_rcd: level=%s, released %zu bytes, remaining=%zu",
           tls_prot_level_name((uint32_t)level), bytes_read,
           c->recv_buf_len_[level]);
    return 1;
}

static int yield_secret_cb(SSL *s, uint32_t prot_level, int direction,
                           const unsigned char *secret, size_t secret_len,
                           void *arg) {
    TlsClient *c = (TlsClient*)arg;
    (void)s;

    LOG_INFO("[tls-client] yield_secret: level=%s dir=%s len=%zu",
           tls_prot_level_name(prot_level), tls_direction_name(direction),
           secret_len);

    if (direction == 0) { /* read */
        c->renc_level_ = prot_level;
    }

    if (c->on_secret_) {
        c->on_secret_(c, prot_level, direction, secret, secret_len);
    }
    return 1;
}

static int got_transport_params_cb(SSL *s, const unsigned char *params,
                                   size_t params_len, void *arg) {
    TlsClient *c = (TlsClient*)arg;
    (void)s;
    LOG_DEBUG("[tls-client] got_transport_params: %zu bytes", params_len);
    return 1;
}

static int alert_cb(SSL *s, unsigned char alert_code, void *arg) {
    TlsClient *c = (TlsClient*)arg;
    (void)s;
    LOG_WARN("[tls-client] alert: code=%u", alert_code);
    return 1;
}

static OSSL_DISPATCH qtdis[] = {
    {OSSL_FUNC_SSL_QUIC_TLS_CRYPTO_SEND,        (void (*)(void))crypto_send_cb},
    {OSSL_FUNC_SSL_QUIC_TLS_CRYPTO_RECV_RCD,    (void (*)(void))crypto_recv_rcd_cb},
    {OSSL_FUNC_SSL_QUIC_TLS_CRYPTO_RELEASE_RCD,  (void (*)(void))crypto_release_rcd_cb},
    {OSSL_FUNC_SSL_QUIC_TLS_YIELD_SECRET,        (void (*)(void))yield_secret_cb},
    {OSSL_FUNC_SSL_QUIC_TLS_GOT_TRANSPORT_PARAMS,(void (*)(void))got_transport_params_cb},
    {OSSL_FUNC_SSL_QUIC_TLS_ALERT,               (void (*)(void))alert_cb},
    {0, NULL}
};

/* ============================================
 * 内部 — UDP 回调
 * ============================================ */

static void udp_on_recv(UdpClient *udp, const char *data, ssize_t nread,
                        const char *from_ip, int from_port) {
    (void)from_ip; (void)from_port;
    TlsClient *c = (TlsClient*)udp->app_data_;
    TlsClientFeedData(c, data, (size_t)nread);
}

static void udp_on_send(UdpClient *udp, int status) {
    if (status < 0) {
        LOG_ERROR("[tls-client] udp send error: %s", uv_strerror(status));
    }
}

static void udp_on_close(UdpClient *udp) {
    TlsClient *c = (TlsClient*)udp->app_data_;
    TlsClientOnClose user_cb = c->on_close_;
    /* 释放 TLS 资源 */
    if (c->ssl_)      SSL_free(c->ssl_);
    if (c->ssl_ctx_)  tls_ctx_free(c->ssl_ctx_);
    for (int i = 0; i < TLS_LEVEL_NUM; i++) free(c->recv_buf_[i]);
    free(c);
    if (user_cb) user_cb(NULL);
}

/* ============================================
 * 公有 — 构造
 * ============================================ */
TlsClient* TlsClientConstruct(uv_loop_t *loop) {
    TlsClient *c = (TlsClient*)calloc(1, sizeof(TlsClient));
    if (!c) return NULL;

    c->loop_ = loop;
    c->renc_level_ = PROT_LEVEL_NONE; /* 初始级别 */

    for (int i = 0; i < TLS_LEVEL_NUM; i++) {
        c->recv_buf_[i] = (unsigned char*)malloc(TLS_RECV_BUF_SIZE);
        if (!c->recv_buf_[i]) {
            for (int j = 0; j < i; j++) free(c->recv_buf_[j]);
            free(c); return NULL;
        }
    }

    c->ssl_ctx_ = tls_ctx_client_new();
    if (!c->ssl_ctx_) {
        for (int i = 0; i < TLS_LEVEL_NUM; i++) free(c->recv_buf_[i]);
        free(c); return NULL;
    }

    c->ssl_ = SSL_new(c->ssl_ctx_);
    if (!c->ssl_) {
        tls_ctx_free(c->ssl_ctx_);
        for (int i = 0; i < TLS_LEVEL_NUM; i++) free(c->recv_buf_[i]);
        free(c); return NULL;
    }

    const unsigned char tp[] = { 0x00 };
    SSL_set_quic_tls_transport_params(c->ssl_, tp, sizeof(tp));
    SSL_set_quic_tls_cbs(c->ssl_, qtdis, c);

    c->udp_ = UdpClientConstruct(loop);
    if (!c->udp_) {
        SSL_free(c->ssl_); tls_ctx_free(c->ssl_ctx_);
        for (int i = 0; i < TLS_LEVEL_NUM; i++) free(c->recv_buf_[i]);
        free(c); return NULL;
    }
    UdpClientSetOnSend(c->udp_, udp_on_send);
    UdpClientSetOnRecv(c->udp_, udp_on_recv);
    UdpClientSetOnClose(c->udp_, udp_on_close);
    c->udp_->app_data_ = c;

    return c;
}

/* ============================================
 * 公有 — 析构
 * ============================================ */
void TlsClientDestruct(TlsClient *c) {
    if (!c) return;
    /* 如果 UDP 层存在，触发异步关闭，资源在 udp_on_close 中释放 */
    if (c->udp_) {
        UdpClientDestruct(c->udp_);
        return;
    }
    /* 无 UDP 层则直接释放 */
    if (c->ssl_)      SSL_free(c->ssl_);
    if (c->ssl_ctx_)  tls_ctx_free(c->ssl_ctx_);
    for (int i = 0; i < TLS_LEVEL_NUM; i++) free(c->recv_buf_[i]);
    free(c);
}

/* ============================================
 * 公有 — 设置回调
 * ============================================ */
void TlsClientSetOnHandshakeDone(TlsClient *c, TlsClientOnHandshakeDone cb) { c->on_handshake_done_ = cb; }
void TlsClientSetOnSecret(TlsClient *c, TlsClientOnSecret cb)               { c->on_secret_ = cb; }
void TlsClientSetOnClose(TlsClient *c, TlsClientOnClose cb)                 { c->on_close_ = cb; }

/* ============================================
 * 公有 — 连接服务器并启动握手
 * ============================================ */
int TlsClientConnect(TlsClient *c, const char *ip, int port) {
    strncpy(c->server_ip_, ip, sizeof(c->server_ip_) - 1);
    c->server_port_ = port;

    int ret = UdpClientBind(c->udp_, "127.0.0.1", 0);
    if (ret < 0) {
        LOG_ERROR("[tls-client] udp bind failed: %s", uv_strerror(ret));
        return ret;
    }

    ret = UdpClientStartRecv(c->udp_);
    if (ret < 0) {
        LOG_ERROR("[tls-client] udp start recv failed: %s", uv_strerror(ret));
        return ret;
    }

    LOG_INFO("[tls-client] connecting to %s:%d", ip, port);

    ret = SSL_connect(c->ssl_);
    if (ret == 1) {
        c->handshake_done_ = 1;
        if (c->on_handshake_done_) c->on_handshake_done_(c, 0);
    } else {
        int err = SSL_get_error(c->ssl_, ret);
        if (err == SSL_ERROR_WANT_READ) {
            /* 正常 */
        } else {
            tls_print_error("SSL_connect");
            if (c->on_handshake_done_) c->on_handshake_done_(c, -1);
            return -1;
        }
    }
    return 0;
}

/* ============================================
 * 公有 — 喂入对端数据，驱动握手
 * ============================================ */
void TlsClientFeedData(TlsClient *c, const char *data, size_t len) {
    if (c->handshake_done_) return;

    /* 追加到当前读级别的缓冲区 */
    int level = (int)c->renc_level_;
    if (c->recv_buf_len_[level] + len > TLS_RECV_BUF_SIZE) {
        LOG_ERROR("[tls-client] recv buffer overflow at level=%s",
                tls_prot_level_name((uint32_t)level));
        return;
    }
    memcpy(c->recv_buf_[level] + c->recv_buf_len_[level], data, len);
    c->recv_buf_len_[level] += len;

    LOG_DEBUG("[tls-client] fed %zu bytes at level=%s, total=%zu",
           len, tls_prot_level_name((uint32_t)level), c->recv_buf_len_[level]);

    int ret = SSL_do_handshake(c->ssl_);
    if (ret == 1) {
        c->handshake_done_ = 1;
        LOG_INFO("[tls-client] handshake done");
        if (c->on_handshake_done_) c->on_handshake_done_(c, 0);
    } else {
        int err = SSL_get_error(c->ssl_, ret);
        if (err == SSL_ERROR_WANT_READ) {
            /* 正常 */
        } else {
            tls_print_error("SSL_do_handshake");
            if (c->on_handshake_done_) c->on_handshake_done_(c, -1);
        }
    }
}
