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

        /* 除自签名根以外的都加入发送链。
         * 判据是「自签名」而不是「是不是最后一个」：bundle 最后一张常常是
         * **交叉签名**的根（本机这份是 TrustAsia TLS RSA Root CA，由 Certum
         * Trusted Network CA 签发），它不是信任锚，而是通向信任锚的那一级 ——
         * 必须发出去，否则对端建不出到信任锚的路径。
         * 浏览器能靠 AIA 回调补链，把这条差异盖住了；rustls（libmoq 客户端）
         * 不做 AIA，直接以 TLS alert unknown_ca（0x130）拒掉。
         * 只有真正的自签名根才不发：它已在对端本地信任库里。 */
        X509_STORE *store = SSL_CTX_get_cert_store(ctx);
        for (int i = 1; i < cert_count; i++) {
            int self_signed =
                X509_NAME_cmp(X509_get_subject_name(certs[i]),
                              X509_get_issuer_name(certs[i])) == 0;
            if (!self_signed) {
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

struct TlsAlpnPref {
    const char **protos;
    int          count;
    /* 0 = 指向静态存储（默认列表），free 时不要动 */
    int          owned;
};

TlsAlpnPref* tls_alpn_pref_new(const char *const *protos, int count) {
    if (!protos || count <= 0) return NULL;

    TlsAlpnPref *p = (TlsAlpnPref*)calloc(1, sizeof(*p));
    if (!p) return NULL;
    p->protos = (const char**)calloc((size_t)count, sizeof(char*));
    if (!p->protos) { free(p); return NULL; }

    /* owned 先置位 —— 下面失败时要靠它让 free 真的释放已 strdup 的那几项 */
    p->owned = 1;
    for (int i = 0; i < count; i++) {
        p->protos[i] = strdup(protos[i]);
        if (!p->protos[i]) { p->count = i; tls_alpn_pref_free(p); return NULL; }
    }
    p->count = count;
    return p;
}

void tls_alpn_pref_free(TlsAlpnPref *pref) {
    if (!pref || !pref->owned) return;
    for (int i = 0; i < pref->count; i++) free((void*)pref->protos[i]);
    free((void*)pref->protos);
    free(pref);
}

static int alpn_select_cb(SSL *ssl, const unsigned char **out,
                          unsigned char *outlen, const unsigned char *in,
                          unsigned int inlen, void *arg) {
    (void)ssl;
    const TlsAlpnPref *pref = (const TlsAlpnPref*)arg;

    /* 客户端 offer 是 length-prefixed 列表。整表按 INFO 打一行 —— 服务端偏好
     * 优先的选择器会掩盖「对端其实没提供我们想要的」这类问题：只看到
     * "selected h3" 无法判断对端到底有没有 offer 过 moq-lite-06。 */
    char offer[256];
    size_t opos = 0;
    offer[0] = '\0';
    unsigned int pos = 0;
    while (pos < inlen && opos + 1 < sizeof(offer)) {
        unsigned char proto_len = in[pos];
        if (pos + 1 + proto_len > inlen) break;
        int n = snprintf(offer + opos, sizeof(offer) - opos, "%s%.*s",
                         opos ? "," : "", proto_len, in + pos + 1);
        if (n < 0) break;
        if ((size_t)n >= sizeof(offer) - opos) { opos = sizeof(offer) - 1; break; }
        opos += (size_t)n;
        pos += 1 + proto_len;
    }
    LOG_INFO("[tls] ALPN offer: [%s]", offer);

    if (!pref || pref->count <= 0) {
        LOG_WARN("[tls] ALPN: no candidate list configured (inlen=%u)", inlen);
        return SSL_TLSEXT_ERR_ALERT_FATAL;
    }

    /* 服务端偏好优先：按候选表顺序挑，第一个客户端也提供的胜出 */
    for (int i = 0; i < pref->count; i++) {
        const char *want = pref->protos[i];
        size_t wlen = strlen(want);
        if (wlen == 0 || wlen > 255) continue;

        pos = 0;
        while (pos < inlen) {
            unsigned char proto_len = in[pos];
            if (pos + 1 + proto_len > inlen) break;
            if ((size_t)proto_len == wlen &&
                memcmp(in + pos + 1, want, wlen) == 0) {
                *out = in + pos + 1;
                *outlen = proto_len;
                LOG_INFO("[tls] ALPN: selected %s", want);
                return SSL_TLSEXT_ERR_OK;
            }
            pos += 1 + proto_len;
        }
    }

    LOG_WARN("[tls] ALPN: no overlap between client offer and server list"
             " (inlen=%u)", inlen);
    return SSL_TLSEXT_ERR_ALERT_FATAL;
}

/* 默认候选表。静态存储，进程生命周期内一直有效 —— 多个监听器共用。 */
static const char *kDefaultAlpnProtos[] = { "h3", "quic-echo" };
static TlsAlpnPref kDefaultAlpnPref = { kDefaultAlpnProtos, 2, 0 };

void tls_ctx_set_alpn(SSL_CTX *ctx) {
    unsigned char protos[] = TLS_ALPN_PROTO;
    if (SSL_CTX_set_alpn_protos(ctx, protos, TLS_ALPN_PROTO_LEN) != 0) {
        LOG_WARN("[tls] SSL_CTX_set_alpn_protos failed");
    }
    SSL_CTX_set_alpn_select_cb(ctx, alpn_select_cb, &kDefaultAlpnPref);
}

void tls_ctx_set_alpn_pref(SSL_CTX *ctx, TlsAlpnPref *pref) {
    if (!ctx || !pref) return;
    SSL_CTX_set_alpn_select_cb(ctx, alpn_select_cb, pref);
}

int tls_alpn_pack(const char *const *protos, int count,
                  unsigned char *out, size_t cap) {
    if (!protos || count <= 0 || !out || cap == 0) return 0;

    size_t off = 0;
    int used = 0;

    for (int i = 0; i < count; i++) {
        const char *p = protos[i];
        if (!p) continue;
        size_t n = strlen(p);
        if (n == 0) continue;
        if (n > 255) {
            LOG_WARN("[tls] ALPN entry too long (%zu bytes), skipped: %.32s...", n, p);
            continue;
        }
        if (off + 1 + n > cap) {
            /* 截断要出声：静默丢弃会让对端"看起来 offer 过"其实没有，
             * 与 §12.1 那个歧义同类，排查时极贵。 */
            LOG_WARN("[tls] ALPN list truncated: kept %d of %d (>%zu bytes)",
                     used, count, cap);
            break;
        }
        out[off++] = (unsigned char)n;
        memcpy(out + off, p, n);
        off += n;
        used++;
    }

    return used > 0 ? (int)off : 0;
}

void tls_ctx_set_alpn_list(SSL_CTX *ctx, const char *const *protos, int count) {
    if (!ctx || !protos || count <= 0) return;

    /* 缓冲开大一点：moq-cli 那种 "Automatic (offer all)" 一次要 offer
     * 十几个版本号（实测 14 个、约 100 字节）。 */
    unsigned char buf[1024];
    int off = tls_alpn_pack(protos, count, buf, sizeof(buf));
    if (off <= 0) {
        LOG_WARN("[tls] ALPN list is empty, keeping the default offer");
        return;
    }

    if (SSL_CTX_set_alpn_protos(ctx, buf, (unsigned int)off) != 0) {
        LOG_WARN("[tls] SSL_CTX_set_alpn_protos failed");
        return;
    }

    {
        char joined[256];
        size_t jp = 0;
        joined[0] = '\0';
        for (int i = 0; i < count && jp < sizeof(joined) - 2; i++) {
            if (!protos[i] || !protos[i][0]) continue;
            int w = snprintf(joined + jp, sizeof(joined) - jp, "%s%s",
                             jp ? "," : "", protos[i]);
            if (w < 0) break;
            jp += (size_t)w;
        }
        LOG_INFO("[tls] client ALPN offer set: [%s]", joined);
    }
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
