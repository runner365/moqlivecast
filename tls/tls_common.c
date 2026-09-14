#include "tls_common.h"
#include "logger.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <openssl/pem.h>
#include <openssl/crypto.h>

/* 全局 cipher 提示词 — 命令行 "--aes256" / "--chacha20" */
static const char *g_cipher_hint = NULL;

void tls_set_cipher_hint(const char *name) { g_cipher_hint = name; }
const char* tls_get_cipher_hint(void)   { return g_cipher_hint; }

const char* tls_prot_level_name(uint32_t level) {
    switch (level) {
    case PROT_LEVEL_NONE:        return "NONE";
    case PROT_LEVEL_EARLY:       return "EARLY(0-RTT)";
    case PROT_LEVEL_HANDSHAKE:   return "HANDSHAKE";
    case PROT_LEVEL_APPLICATION: return "APPLICATION(1-RTT)";
    default:                     return "UNKNOWN";
    }
}

const char* tls_direction_name(int direction) {
    return direction == 0 ? "read" : "write";
}

void tls_print_error(const char *tag) {
    unsigned long err = ERR_get_error();
    char buf[256];
    ERR_error_string_n(err, buf, sizeof(buf));
    LOG_ERROR("[%s] TLS error: %s", tag, buf);
}

SSL_CTX* tls_ctx_client_new(void) {
    return tls_ctx_client_new_cipher(g_cipher_hint);
}

SSL_CTX* tls_ctx_client_new_cipher(const char *cipher_name) {
    SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());
    if (!ctx) {
        tls_print_error("SSL_CTX_new(client)");
        return NULL;
    }
    SSL_CTX_set_min_proto_version(ctx, TLS1_3_VERSION);
    SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, NULL);
    if (cipher_name) tls_ctx_set_ciphersuite(ctx, cipher_name);
    return ctx;
}

SSL_CTX* tls_ctx_server_new(const char *cert_file, const char *key_file) {
    return tls_ctx_server_new_cipher(cert_file, key_file, g_cipher_hint);
}

SSL_CTX* tls_ctx_server_new_cipher(const char *cert_file, const char *key_file,
                                    const char *cipher_name) {
    SSL_CTX *ctx = SSL_CTX_new(TLS_server_method());
    if (!ctx) {
        tls_print_error("SSL_CTX_new(server)");
        return NULL;
    }
    LOG_INFO("[tls] OpenSSL version: %s", OpenSSL_version(OPENSSL_VERSION));
    LOG_INFO("[tls] OpenSSL full: %s", OpenSSL_version(OPENSSL_FULL_VERSION_STRING));
    SSL_CTX_set_min_proto_version(ctx, TLS1_3_VERSION);

    /* 显式加载证书链：bundle 文件里第一个是 leaf，后续是中间 CA + 根 CA。
     * use_certificate_chain_file 在 OpenSSL QUIC 模式下对链的序列化不可靠。
     * 这里手动逐个加载：
     *   - leaf 用 use_certificate
     *   - 中间 CA 加入发送链（cpk->chain）
     *   - 根 CA 只加 cert_store 作为信任锚、不加入发送链
     *     （根 CA 由客户端本地信任库提供，服务端不应发送） */
    {
        FILE *fp = fopen(cert_file, "r");
        if (!fp) {
            tls_print_error("fopen cert_file");
            SSL_CTX_free(ctx);
            return NULL;
        }

        /* 先读所有证书到数组 */
        X509 *certs[16];
        int cert_count = 0;
        X509 *c;
        while ((c = PEM_read_X509(fp, NULL, NULL, NULL)) != NULL && cert_count < 16) {
            certs[cert_count++] = c;
        }
        fclose(fp);

        if (cert_count < 1) {
            tls_print_error("no certificate in bundle");
            SSL_CTX_free(ctx);
            return NULL;
        }

        /* 第一个是 leaf */
        if (SSL_CTX_use_certificate(ctx, certs[0]) != 1) {
            tls_print_error("SSL_CTX_use_certificate (leaf)");
            for (int i = 0; i < cert_count; i++) X509_free(certs[i]);
            SSL_CTX_free(ctx);
            return NULL;
        }
        X509_free(certs[0]);

        /* 中间 CA（第 2 到倒数第 2 个）加入发送链 */
        /* 最后一个（根 CA）只加 cert_store */
        X509_STORE *store = SSL_CTX_get_cert_store(ctx);
        for (int i = 1; i < cert_count; i++) {
            int is_root = (i == cert_count - 1);
            if (!is_root) {
                if (SSL_CTX_add1_chain_cert(ctx, certs[i]) != 1) {
                    tls_print_error("SSL_CTX_add1_chain_cert");
                    for (int j = i; j < cert_count; j++) X509_free(certs[j]);
                    SSL_CTX_free(ctx);
                    return NULL;
                }
            }
            /* 根 CA 和中间 CA 都加入 cert_store（信任锚） */
            if (store && X509_STORE_add_cert(store, certs[i]) != 1) {
                tls_print_error("X509_STORE_add_cert");
            }
            X509_free(certs[i]);
        }
    }

    /* 验证 chain 是否加载成功 */
    {
        STACK_OF(X509) *chain = NULL;
        SSL_CTX_get0_chain_certs(ctx, &chain);
        int n = chain ? sk_X509_num(chain) : -1;
        LOG_INFO("[tls] cert chain loaded: %d certs", n);
        if (chain) {
            for (int i = 0; i < n; i++) {
                X509 *x = sk_X509_value(chain, i);
                char subj[256] = {0};
                X509_NAME_oneline(X509_get_subject_name(x), subj, sizeof(subj));
                LOG_INFO("[tls]   chain[%d]: %s", i, subj);
            }
        }
    }

    if (SSL_CTX_use_PrivateKey_file(ctx, key_file, SSL_FILETYPE_PEM) != 1) {
        tls_print_error("SSL_CTX_use_PrivateKey_file");
        SSL_CTX_free(ctx);
        return NULL;
    }
    if (SSL_CTX_check_private_key(ctx) != 1) {
        tls_print_error("SSL_CTX_check_private_key");
        SSL_CTX_free(ctx);
        return NULL;
    }
    if (cipher_name) tls_ctx_set_ciphersuite(ctx, cipher_name);
    return ctx;
}

static int alpn_select_cb(SSL *ssl, const unsigned char **out,
                          unsigned char *outlen, const unsigned char *in,
                          unsigned int inlen, void *arg) {
    (void)ssl; (void)arg;
    /* 遍历客户端提供的协议列表（length-prefixed），选择 "quic-echo" */
    unsigned int pos = 0;
    while (pos < inlen) {
        unsigned char proto_len = in[pos];
        if (pos + 1 + proto_len > inlen) break;
        LOG_DEBUG("[tls] ALPN offer[%u]: %.*s (plen=%u, in+%u=%.8s)",
                  pos, proto_len, in + pos + 1, proto_len, pos + 1, in + pos + 1);
        /* 优先选择 "h3"（标准 HTTP/3），其次 "quic-echo"（传统 echo） */
        if (proto_len == 2 && memcmp(in + pos + 1, "h3", 2) == 0) {
            *out = in + pos + 1;
            *outlen = proto_len;
            LOG_DEBUG("[tls] ALPN: selected h3");
            return SSL_TLSEXT_ERR_OK;
        }
        if (proto_len == 9 && memcmp(in + pos + 1, "quic-echo", 9) == 0) {
            *out = in + pos + 1;
            *outlen = proto_len;
            LOG_DEBUG("[tls] ALPN: selected quic-echo");
            return SSL_TLSEXT_ERR_OK;
        }
        pos += 1 + proto_len;
    }
    LOG_WARN("[tls] ALPN: client did not offer quic-echo (inlen=%u)", inlen);
    return SSL_TLSEXT_ERR_ALERT_FATAL;
}

void tls_ctx_set_alpn(SSL_CTX *ctx) {
    unsigned char protos[] = TLS_ALPN_PROTO;
    if (SSL_CTX_set_alpn_protos(ctx, protos, TLS_ALPN_PROTO_LEN) != 0) {
        LOG_WARN("[tls] SSL_CTX_set_alpn_protos failed");
    }
    SSL_CTX_set_alpn_select_cb(ctx, alpn_select_cb, NULL);
}

int tls_ctx_set_ciphersuite(SSL_CTX *ctx, const char *name) {
    if (!name) return 0;  /* NULL = 恢复默认 */

    /* TLS 1.3 cipher suite 通过 SSL_CTX_set_ciphersuites 配置 */
    if (SSL_CTX_set_ciphersuites(ctx, name) != 1) {
        LOG_ERROR("[tls] SSL_CTX_set_ciphersuites(%s) failed", name);
        return -1;
    }
    LOG_DEBUG("[tls] restricted cipher suite to: %s", name);
    return 0;
}

void tls_ctx_free(SSL_CTX *ctx) {
    if (ctx) SSL_CTX_free(ctx);
}
