#ifndef QUIC_CRYPTO_H
#define QUIC_CRYPTO_H

#include "quic_common.h"
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================
 * 密钥派生 (RFC 9001 §5)
 * ============================================ */

int quic_crypto_derive_initial_keys(QuicCipherKeys *client_write,
                                    QuicCipherKeys *server_write,
                                    const uint8_t *dcid, size_t dcid_len);

int quic_crypto_derive_from_secret(QuicCipherKeys *keys,
                                   const uint8_t *secret, size_t secret_len);

/* ============================================
 * AEAD 加解密 (RFC 9001 §5.3)
 * ============================================ */

int quic_crypto_encrypt(QuicCipherKeys *keys, uint64_t pn,
                        const uint8_t *aad, size_t aad_len,
                        uint8_t *plaintext, size_t plaintext_len,
                        uint8_t *out, size_t *out_len);

int quic_crypto_decrypt(QuicCipherKeys *keys, uint64_t pn,
                        const uint8_t *aad, size_t aad_len,
                        const uint8_t *in, size_t in_len,
                        uint8_t *out, size_t *out_len);

/* ============================================
 * Header Protection (RFC 9001 §5.4)
 * ============================================ */

/* 应用 header protection（出站包）— pn_len 已知 */
void quic_crypto_hp_apply(QuicCipherKeys *keys,
                          uint8_t *pkt, size_t pkt_len,
                          size_t pn_offset, size_t pn_len);

/* 移除 header protection（长头入站包）。
 * 内部解决 pn_len 鸡生蛋问题：先还原第一字节获取 pn_len，再还原 PN。
 * 返回实际 pn_len (1-4)。失败返回 0。 */
int quic_crypto_hp_remove_long(QuicCipherKeys *keys,
                                uint8_t *pkt, size_t pkt_len,
                                size_t pn_offset);

/* 移除 header protection（短头入站包）。同上。 */
int quic_crypto_hp_remove_short(QuicCipherKeys *keys,
                                 uint8_t *pkt, size_t pkt_len,
                                 size_t pn_offset);

/* ============================================
 * Key Update (RFC 9001 §6)
 * ============================================ */

/* 从 current_secret 派生下一代密钥。
 * new_secret_out 输出新的应用流量密钥（调用方保存用于后续更新）。
 * 返回 0 成功，<0 失败。 */
int quic_crypto_derive_key_update(QuicCipherKeys *keys,
                                   const uint8_t *current_secret, size_t secret_len,
                                   uint8_t *new_secret_out, size_t new_secret_cap);

/* 同上，但只更新 AEAD key+iv，保留原有的 hp_key。
 * quic-go 的 rollKeys() 不轮转 header protector。
 * 返回 0 成功，<0 失败。 */
int quic_crypto_derive_key_update_aead(QuicCipherKeys *keys,
                                        const uint8_t *current_secret, size_t secret_len,
                                        uint8_t *new_secret_out, size_t new_secret_cap);

/* ============================================
 * Cipher Suite (RFC 9001 §5.1)
 * ============================================ */

typedef enum {
    QUIC_CIPHER_AES_128_GCM = 0,
    QUIC_CIPHER_AES_256_GCM,
    QUIC_CIPHER_CHACHA20_POLY1305,
} QuicCipherSuite;

/* 从 SSL 对象读取 TLS 协商结果，设置全局 cipher suite。
 * 必须在 yield_secret_cb 之前调用（即在握手完成后）。 */
void quic_crypto_set_cipher_suite_from_ssl(void *ssl_ptr);

/* 查询当前协商的 cipher suite */
QuicCipherSuite quic_crypto_get_suite(void);

/* Retry 完整性标签 (RFC 9001 §5.8) — AES-128-GCM over pseudo-packet */
int  quic_crypto_compute_retry_tag(const uint8_t *pseudo_pkt, size_t pseudo_len,
                                    uint8_t tag_out[16]);

/* ============================================
 * 模块初始化 / 清理
 * ============================================ */

void quic_crypto_init(void);
void quic_crypto_cleanup(void);

#ifdef __cplusplus
}
#endif

#endif /* QUIC_CRYPTO_H */
