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

/* ALPN — QUIC 要求必须设置。默认列表：标准 HTTP/3 + echo 测试 */
#define TLS_ALPN_PROTO "\x02h3\x09quic-echo"
#define TLS_ALPN_PROTO_LEN 13

/* 服务端 ALPN 候选列表。
 *
 * 一个进程里可以有多个监听器，各自要不同的 ALPN（WebTransport 走 h3，
 * 裸 QUIC 的 moq-lite 走 moq-lite-06），而选择回调是按 SSL_CTX 注册的，
 * 所以列表必须**按 ctx 携带** —— 用这个对象当载体。
 *
 * 生命周期由调用方管：pref 必须比所有用到它的 SSL_CTX 活得久。多个监听器
 * 可以共用同一个 pref。
 *
 * 选择规则是**服务端偏好优先**：按 protos 的先后顺序取第一个客户端也提供的；
 * 客户端 offer 的顺序不影响结果。 */
typedef struct TlsAlpnPref TlsAlpnPref;

/* protos 是 C 字符串数组（内部转成 OpenSSL 要的 length-prefixed 格式）。
 * 返回 NULL 表示入参非法或内存不足。 */
TlsAlpnPref* tls_alpn_pref_new(const char *const *protos, int count);
void         tls_alpn_pref_free(TlsAlpnPref *pref);

/* SSL_CTX 工厂 — cipher_name 为 NULL 使用默认，非 NULL 强制指定套件 */
SSL_CTX* tls_ctx_client_new(void);
SSL_CTX* tls_ctx_server_new(const char *cert_file, const char *key_file);

/* 带 cipher suite 限制的工厂 */
SSL_CTX* tls_ctx_client_new_cipher(const char *cipher_name);
SSL_CTX* tls_ctx_server_new_cipher(const char *cert_file, const char *key_file,
                                    const char *cipher_name);

/* ALPN 设置 — 默认列表（h3 + quic-echo），同时设客户端 offer 和服务端选择回调 */
void tls_ctx_set_alpn(SSL_CTX *ctx);

/* 用指定候选列表替换服务端的选择回调。只影响本 ctx，**不改默认值**，
 * 所以 WebTransport 那条路（默认列表）不受调用本函数影响。
 * 客户端 offer 那份不设 —— 服务端不用它。 */
void tls_ctx_set_alpn_pref(SSL_CTX *ctx, TlsAlpnPref *pref);

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
