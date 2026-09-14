#ifndef TLS_COMMON_H
#define TLS_COMMON_H
#include <openssl/ssl.h>
#include <openssl/err.h>

#define TLS_RECV_BUF_SIZE 65536
#define TLS_LEVEL_NUM      4

/* 保护级别 — OpenSSL 公开 API (ssl.h) */
#define PROT_LEVEL_NONE        OSSL_RECORD_PROTECTION_LEVEL_NONE
#define PROT_LEVEL_EARLY       OSSL_RECORD_PROTECTION_LEVEL_EARLY
#define PROT_LEVEL_HANDSHAKE   OSSL_RECORD_PROTECTION_LEVEL_HANDSHAKE
#define PROT_LEVEL_APPLICATION OSSL_RECORD_PROTECTION_LEVEL_APPLICATION

const char* tls_prot_level_name(uint32_t level);
const char* tls_direction_name(int direction);

/* ALPN — QUIC 要求必须设置。支持标准 HTTP/3 和 echo 测试 */
#define TLS_ALPN_PROTO "\x02h3\x09quic-echo"
#define TLS_ALPN_PROTO_LEN 13

/* SSL_CTX 工厂 — cipher_name 为 NULL 使用默认，非 NULL 强制指定套件 */
SSL_CTX* tls_ctx_client_new(void);
SSL_CTX* tls_ctx_server_new(const char *cert_file, const char *key_file);

/* 带 cipher suite 限制的工厂 */
SSL_CTX* tls_ctx_client_new_cipher(const char *cipher_name);
SSL_CTX* tls_ctx_server_new_cipher(const char *cert_file, const char *key_file,
                                    const char *cipher_name);

/* ALPN 设置 */
void tls_ctx_set_alpn(SSL_CTX *ctx);

/* 限制 TLS 1.3 cipher suite（仅用于测试 cipher negotiation 路径）。
 * name 如 "TLS_AES_256_GCM_SHA384"、NULL 恢复默认。
 * 返回 0 成功，<0 失败。 */
int  tls_ctx_set_ciphersuite(SSL_CTX *ctx, const char *name);

/* 设置全局 cipher suite 提示词（在创建 SSL_CTX 前调用生效）。
 * 命令行传入 "--aes256" 或 "--chacha20"，不传默认 AES-128。 */
void tls_set_cipher_hint(const char *name);
const char* tls_get_cipher_hint(void);

void tls_ctx_free(SSL_CTX *ctx);
void     tls_print_error(const char *tag);

#endif
