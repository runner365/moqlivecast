#include "tls_server.h"
#include "tls_common.h"
#include "udp_server.h"
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
    TlsServer *svr = (TlsServer*)arg;
    (void)s;

    int ret = UdpServerSend(svr->udp_, (const char*)buf, buf_len,
                            svr->client_ip_, svr->client_port_);
    if (ret < 0) {
        LOG_ERROR("[tls-server] crypto_send error: %s", uv_strerror(ret));
        return 0;
    }
    *consumed = buf_len;
    LOG_DEBUG("[tls-server] crypto_send: %zu bytes to %s:%d",
           buf_len, svr->client_ip_, svr->client_port_);
    return 1;
}

static int crypto_recv_rcd_cb(SSL *s, const unsigned char **buf,
                              size_t *bytes_read, void *arg) {
    TlsServer *svr = (TlsServer*)arg;
    (void)s;

    int level = (int)svr->renc_level_;
    if (svr->recv_buf_len_[level] < 21) {
        *buf        = NULL;
        *bytes_read = 0;
        LOG_DEBUG("[tls-server] crypto_recv_rcd: level=%s, %zu bytes (waiting for more)",
               tls_prot_level_name((uint32_t)level), svr->recv_buf_len_[level]);
        return 1;
    }

    *buf        = svr->recv_buf_[level];
    *bytes_read = svr->recv_buf_len_[level];
    LOG_DEBUG("[tls-server] crypto_recv_rcd: level=%s, returning %zu bytes",
           tls_prot_level_name((uint32_t)level), *bytes_read);
    return 1;
}

static int crypto_release_rcd_cb(SSL *s, size_t bytes_read, void *arg) {
    TlsServer *svr = (TlsServer*)arg;
    (void)s;

    int level = (int)svr->renc_level_;
    if (bytes_read > 0 && bytes_read <= svr->recv_buf_len_[level]) {
        size_t remaining = svr->recv_buf_len_[level] - bytes_read;
        if (remaining > 0) {
            memmove(svr->recv_buf_[level],
                    svr->recv_buf_[level] + bytes_read, remaining);
        }
        svr->recv_buf_len_[level] = remaining;
    }
    LOG_DEBUG("[tls-server] crypto_release_rcd: level=%s, released %zu bytes, remaining=%zu",
           tls_prot_level_name((uint32_t)level), bytes_read,
           svr->recv_buf_len_[level]);
    return 1;
}

static int yield_secret_cb(SSL *s, uint32_t prot_level, int direction,
                           const unsigned char *secret, size_t secret_len,
                           void *arg) {
    TlsServer *svr = (TlsServer*)arg;
    (void)s;

    LOG_INFO("[tls-server] yield_secret: level=%s dir=%s len=%zu",
           tls_prot_level_name(prot_level), tls_direction_name(direction),
           secret_len);

    if (direction == 0) { /* read */
        svr->renc_level_ = prot_level;
    }

    if (svr->on_secret_) {
        svr->on_secret_(svr, prot_level, direction, secret, secret_len);
    }
    return 1;
}

static int got_transport_params_cb(SSL *s, const unsigned char *params,
                                   size_t params_len, void *arg) {
    TlsServer *svr = (TlsServer*)arg;
    (void)s;
    LOG_DEBUG("[tls-server] got_transport_params: %zu bytes", params_len);
    return 1;
}

static int alert_cb(SSL *s, unsigned char alert_code, void *arg) {
    TlsServer *svr = (TlsServer*)arg;
    (void)s;
    LOG_WARN("[tls-server] alert: code=%u", alert_code);
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

static void udp_on_recv(UdpServer *udp, const char *data, ssize_t nread,
                        const char *from_ip, int from_port) {
    TlsServer *svr = (TlsServer*)udp->app_data_;

    strncpy(svr->client_ip_, from_ip, sizeof(svr->client_ip_) - 1);
    svr->client_port_ = from_port;

    if (svr->handshake_done_) return;

    int level = (int)svr->renc_level_;
    if (svr->recv_buf_len_[level] + (size_t)nread > TLS_RECV_BUF_SIZE) {
        LOG_ERROR("[tls-server] recv buffer overflow at level=%s",
                tls_prot_level_name((uint32_t)level));
        return;
    }
    memcpy(svr->recv_buf_[level] + svr->recv_buf_len_[level], data, (size_t)nread);
    svr->recv_buf_len_[level] += (size_t)nread;

    LOG_DEBUG("[tls-server] fed %zd bytes at level=%s, total=%zu",
           nread, tls_prot_level_name((uint32_t)level), svr->recv_buf_len_[level]);

    int ret = SSL_accept(svr->ssl_);
    if (ret == 1) {
        svr->handshake_done_ = 1;
        LOG_INFO("[tls-server] handshake done");
        if (svr->on_handshake_done_) svr->on_handshake_done_(svr, 0);
    } else {
        int err = SSL_get_error(svr->ssl_, ret);
        if (err == SSL_ERROR_WANT_READ) {
            /* 正常 */
        } else {
            tls_print_error("SSL_accept");
            if (svr->on_handshake_done_) svr->on_handshake_done_(svr, -1);
        }
    }
}

static void udp_on_send(UdpServer *udp, int status) {
    if (status < 0) {
        LOG_ERROR("[tls-server] udp send error: %s", uv_strerror(status));
    }
}

static void udp_on_error(UdpServer *udp, int errcode) {
    LOG_ERROR("[tls-server] udp error: %s", uv_strerror(errcode));
}

static void udp_on_close(UdpServer *udp) {
    TlsServer *svr = (TlsServer*)udp->app_data_;
    TlsServerOnClose user_cb = svr->on_close_;
    if (svr->ssl_)      SSL_free(svr->ssl_);
    if (svr->ssl_ctx_)  tls_ctx_free(svr->ssl_ctx_);
    for (int i = 0; i < TLS_LEVEL_NUM; i++) free(svr->recv_buf_[i]);
    free(svr);
    if (user_cb) user_cb(NULL);
}

/* ============================================
 * 公有 — 构造
 * ============================================ */
TlsServer* TlsServerConstruct(uv_loop_t *loop,
                               const char *cert_file, const char *key_file) {
    TlsServer *s = (TlsServer*)calloc(1, sizeof(TlsServer));
    if (!s) return NULL;

    s->loop_       = loop;
    s->renc_level_ = PROT_LEVEL_NONE;

    for (int i = 0; i < TLS_LEVEL_NUM; i++) {
        s->recv_buf_[i] = (unsigned char*)malloc(TLS_RECV_BUF_SIZE);
        if (!s->recv_buf_[i]) {
            for (int j = 0; j < i; j++) free(s->recv_buf_[j]);
            free(s); return NULL;
        }
    }

    s->ssl_ctx_ = tls_ctx_server_new(cert_file, key_file);
    if (!s->ssl_ctx_) {
        for (int i = 0; i < TLS_LEVEL_NUM; i++) free(s->recv_buf_[i]);
        free(s); return NULL;
    }

    s->ssl_ = SSL_new(s->ssl_ctx_);
    if (!s->ssl_) {
        tls_ctx_free(s->ssl_ctx_);
        for (int i = 0; i < TLS_LEVEL_NUM; i++) free(s->recv_buf_[i]);
        free(s); return NULL;
    }

    const unsigned char tp[] = { 0x00 };
    SSL_set_quic_tls_transport_params(s->ssl_, tp, sizeof(tp));
    SSL_set_quic_tls_cbs(s->ssl_, qtdis, s);

    s->udp_ = UdpServerConstruct(loop);
    if (!s->udp_) {
        SSL_free(s->ssl_); tls_ctx_free(s->ssl_ctx_);
        for (int i = 0; i < TLS_LEVEL_NUM; i++) free(s->recv_buf_[i]);
        free(s); return NULL;
    }
    UdpServerSetOnRecv(s->udp_, udp_on_recv);
    UdpServerSetOnSend(s->udp_, udp_on_send);
    UdpServerSetOnError(s->udp_, udp_on_error);
    UdpServerSetOnClose(s->udp_, udp_on_close);
    s->udp_->app_data_ = s;

    return s;
}

/* ============================================
 * 公有 — 析构
 * ============================================ */
void TlsServerDestruct(TlsServer *s) {
    if (!s) return;
    if (s->udp_) {
        UdpServerDestruct(s->udp_);
        return;
    }
    if (s->ssl_)      SSL_free(s->ssl_);
    if (s->ssl_ctx_)  tls_ctx_free(s->ssl_ctx_);
    for (int i = 0; i < TLS_LEVEL_NUM; i++) free(s->recv_buf_[i]);
    free(s);
}

/* ============================================
 * 公有 — 设置回调
 * ============================================ */
void TlsServerSetOnHandshakeDone(TlsServer *s, TlsServerOnHandshakeDone cb) { s->on_handshake_done_ = cb; }
void TlsServerSetOnSecret(TlsServer *s, TlsServerOnSecret cb)               { s->on_secret_ = cb; }
void TlsServerSetOnClose(TlsServer *s, TlsServerOnClose cb)                 { s->on_close_ = cb; }

/* ============================================
 * 公有 — 启动监听
 * ============================================ */
int TlsServerListen(TlsServer *s, const char *ip, int port) {
    int ret = UdpServerBind(s->udp_, ip, port);
    if (ret < 0) {
        LOG_ERROR("[tls-server] bind failed: %s", uv_strerror(ret));
        return ret;
    }

    ret = UdpServerStartRecv(s->udp_);
    if (ret < 0) {
        LOG_ERROR("[tls-server] start recv failed: %s", uv_strerror(ret));
        return ret;
    }

    LOG_INFO("[tls-server] listening on %s:%d", ip, port);
    return 0;
}
