#include "quic_crypto.h"
#include "logger.h"
#include <openssl/evp.h>
#include <openssl/kdf.h>
#include <openssl/ssl.h>
#include <openssl/core_names.h>
#include <string.h>
#include <stdlib.h>

/* RFC 9001 §5.2 — initial salt for QUIC v1 */
static const uint8_t QUIC_V1_SALT[20] = {
    0x38, 0x76, 0x2c, 0xf7, 0xf5, 0x59, 0x34, 0xb3,
    0x4d, 0x17, 0x9a, 0xe6, 0xa4, 0xc8, 0x0c, 0xad,
    0xcc, 0xbb, 0x7f, 0x0a
};

/* 缓存的 cipher 对象 — 三套 cipher suite */
static EVP_CIPHER *g_aead[3] = {NULL};  /* AES-128-GCM, AES-256-GCM, ChaCha20 */
static EVP_CIPHER *g_hp[3]   = {NULL};  /* AES-128-ECB, AES-256-ECB, ChaCha20 */

/* 当前协商的 cipher suite */
static QuicCipherSuite g_suite = QUIC_CIPHER_AES_128_GCM;

/* 各 suite 的 AEAD key 长度 */
static const int g_key_len[3] = {16, 32, 32};

static const char *get_digest_name(int suite_idx) {
    switch (suite_idx) {
    case 0:  return "SHA-256";   /* AES-128-GCM */
    case 1:  return "SHA-384";   /* AES-256-GCM */
    case 2:  return "SHA-256";   /* ChaCha20-Poly1305 */
    default: return "SHA-256";
    }
}

/* ============================================
 * 模块初始化 / 清理
 * ============================================ */

void quic_crypto_init(void) {
    if (!g_aead[0]) g_aead[0] = EVP_CIPHER_fetch(NULL, "AES-128-GCM", NULL);
    if (!g_hp[0])   g_hp[0]   = EVP_CIPHER_fetch(NULL, "AES-128-ECB", NULL);
    if (!g_aead[1]) g_aead[1] = EVP_CIPHER_fetch(NULL, "AES-256-GCM", NULL);
    if (!g_hp[1])   g_hp[1]   = EVP_CIPHER_fetch(NULL, "AES-256-ECB", NULL);
    if (!g_aead[2]) g_aead[2] = EVP_CIPHER_fetch(NULL, "ChaCha20-Poly1305", NULL);
    if (!g_hp[2])   g_hp[2]   = EVP_CIPHER_fetch(NULL, "ChaCha20", NULL);
}

void quic_crypto_cleanup(void) {
    for (int i = 0; i < 3; i++) {
        if (g_aead[i]) { EVP_CIPHER_free(g_aead[i]); g_aead[i] = NULL; }
        if (g_hp[i])   { EVP_CIPHER_free(g_hp[i]);   g_hp[i]   = NULL; }
    }
}

/* ============================================
 * Cipher Suite 协商
 * ============================================ */

void quic_crypto_set_cipher_suite_from_ssl(void *ssl_ptr) {
    SSL *ssl = (SSL*)ssl_ptr;
    const char *name = SSL_CIPHER_get_name(SSL_get_current_cipher(ssl));
    if (!name) return;

    if (strstr(name, "AES-256-GCM") || strstr(name, "AES_256_GCM"))
        g_suite = QUIC_CIPHER_AES_256_GCM;
    else if (strstr(name, "CHACHA20") || strstr(name, "chacha20"))
        g_suite = QUIC_CIPHER_CHACHA20_POLY1305;
    else
        g_suite = QUIC_CIPHER_AES_128_GCM;  /* default / fallback */

    LOG_DEBUG("[quic-crypto] TLS negotiated cipher: %s → suite=%d key_len=%d",
             name, (int)g_suite, g_key_len[g_suite]);
}

QuicCipherSuite quic_crypto_get_suite(void) {
    return g_suite;
}

/* ============================================
 * HKDF (RFC 5869 / RFC 8446 §7.1)
 * ============================================ */

static int hkdf_extract_digest(const char *digest,
                               const uint8_t *salt, size_t salt_len,
                               const uint8_t *ikm, size_t ikm_len,
                               uint8_t *out, size_t out_len) {
    EVP_KDF *kdf = EVP_KDF_fetch(NULL, "HKDF", NULL);
    if (!kdf) { LOG_ERROR("[quic-crypto] EVP_KDF_fetch(HKDF) failed"); return -1; }

    EVP_KDF_CTX *kctx = EVP_KDF_CTX_new(kdf);
    EVP_KDF_free(kdf);
    if (!kctx) { LOG_ERROR("[quic-crypto] EVP_KDF_CTX_new failed"); return -1; }

    OSSL_PARAM params[5], *p = params;
    *p++ = OSSL_PARAM_construct_utf8_string(OSSL_KDF_PARAM_DIGEST,
           (char*)digest, 0);
    *p++ = OSSL_PARAM_construct_int(OSSL_KDF_PARAM_MODE,
           (int*)&(int){EVP_KDF_HKDF_MODE_EXTRACT_ONLY});
    if (salt && salt_len > 0) {
        *p++ = OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_SALT,
               (void*)salt, salt_len);
    }
    *p++ = OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_KEY,
           (void*)ikm, ikm_len);
    *p = OSSL_PARAM_construct_end();

    int ret = EVP_KDF_derive(kctx, out, out_len, params);
    EVP_KDF_CTX_free(kctx);
    return (ret > 0) ? 0 : -1;
}

static int hkdf_expand_label_digest(const char *digest,
                                    const uint8_t *secret, size_t secret_len,
                                    const char *label,
                                    const uint8_t *context, size_t context_len,
                                    uint8_t *out, size_t out_len) {
    /* HkdfLabel = struct {
     *   uint16 length;
     *   opaque label<7..255> = "tls13 " + Label;
     *   opaque context<0..255> = Context;
     * }
     * 手动构造 HkdfLabel，然后用 HKDF-Expand 派生。 */
    const char *prefix = "tls13 ";
    size_t prefix_len = 6;
    size_t label_len = strlen(label);
    size_t hkdf_label_len = 2 + 1 + prefix_len + label_len + 1 + context_len;

    uint8_t hkdf_label[256];
    if (hkdf_label_len > sizeof(hkdf_label)) return -1;

    size_t pos = 0;
    /* length (uint16 big-endian) */
    hkdf_label[pos++] = (uint8_t)(out_len >> 8);
    hkdf_label[pos++] = (uint8_t)(out_len);

    /* label (1-byte length prefix + content) */
    hkdf_label[pos++] = (uint8_t)(prefix_len + label_len);
    memcpy(hkdf_label + pos, prefix, prefix_len); pos += prefix_len;
    memcpy(hkdf_label + pos, label, label_len);   pos += label_len;

    /* context (1-byte length prefix + content) */
    hkdf_label[pos++] = (uint8_t)context_len;
    if (context_len > 0) {
        memcpy(hkdf_label + pos, context, context_len);
        pos += context_len;
    }

    /* HKDF-Expand */
    EVP_KDF *kdf = EVP_KDF_fetch(NULL, "HKDF", NULL);
    if (!kdf) { LOG_ERROR("[quic-crypto] EVP_KDF_fetch(HKDF) failed"); return -1; }

    EVP_KDF_CTX *kctx = EVP_KDF_CTX_new(kdf);
    EVP_KDF_free(kdf);
    if (!kctx) return -1;

    OSSL_PARAM params[6], *p = params;
    *p++ = OSSL_PARAM_construct_utf8_string(OSSL_KDF_PARAM_DIGEST,
           (char*)digest, 0);
    *p++ = OSSL_PARAM_construct_int(OSSL_KDF_PARAM_MODE,
           (int*)&(int){EVP_KDF_HKDF_MODE_EXPAND_ONLY});
    *p++ = OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_KEY,
           (void*)secret, secret_len);
    *p++ = OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_INFO,
           hkdf_label, pos);
    *p = OSSL_PARAM_construct_end();

    int ret = EVP_KDF_derive(kctx, out, out_len, params);
    EVP_KDF_CTX_free(kctx);
    return (ret > 0) ? 0 : -1;
}

/* ============================================
 * 调试辅助
 * ============================================ */
static void hex_dump(const char *tag, const uint8_t *data, size_t len) {
    char buf[4096];
    size_t pos = 0;
    size_t dump_len = len < 2048 ? len : 2048;
    for (size_t i = 0; i < dump_len && pos < sizeof(buf) - 3; i++) {
        pos += snprintf(buf + pos, sizeof(buf) - pos, "%02x", data[i]);
    }
    buf[pos] = '\0';
    LOG_DEBUG("[quic-crypto] %s[%zu] = %s", tag, len, buf);
}

/* ============================================
 * 密钥派生
 * ============================================ */

static int derive_quic_keys(QuicCipherKeys *keys,
                            const uint8_t *secret, size_t secret_len) {
    /* 使用当前协商的 cipher suite 确定密钥长度 */
    int suite_idx = (int)g_suite;
    int klen = g_key_len[suite_idx];
    keys->key_len = klen;
    keys->suite   = suite_idx;

    uint8_t ok = (uint8_t)(hkdf_expand_label_digest(get_digest_name(suite_idx),
                                                     secret, secret_len,
                                                     "quic key",
                                                     NULL, 0,
                                                     keys->key, klen) == 0);
    ok &= (uint8_t)(hkdf_expand_label_digest(get_digest_name(suite_idx),
                                             secret, secret_len, "quic iv",
                                             NULL, 0, keys->iv, 12) == 0);
    ok &= (uint8_t)(hkdf_expand_label_digest(get_digest_name(suite_idx),
                                             secret, secret_len, "quic hp",
                                             NULL, 0,
                                             keys->hp_key, klen) == 0);
    keys->initialized = ok ? 1 : 0;
    if (ok) {
        LOG_DEBUG("[quic-crypto] derived %d-byte keys suite=%d digest=%s"
             " key=%02x%02x.. iv=%02x%02x.. hp=%02x%02x..",
             klen, suite_idx, get_digest_name(suite_idx),
             keys->key[0], keys->key[1],
             keys->iv[0], keys->iv[1],
             keys->hp_key[0], keys->hp_key[1]);
    }
    return ok ? 0 : -1;
}

int quic_crypto_derive_initial_keys(QuicCipherKeys *client_write,
                                    QuicCipherKeys *server_write,
                                    const uint8_t *dcid, size_t dcid_len) {
    /* RFC 9001 §5.2: Initial keys are ALWAYS AES-128-GCM,
     * regardless of negotiated cipher suite. */
    QuicCipherSuite saved_suite = g_suite;
    g_suite = QUIC_CIPHER_AES_128_GCM;

    hex_dump("derive_initial: dcid", dcid, dcid_len);

    /* initial_secret = HKDF-Extract(initial_salt, dcid) — RFC 9001 §5.2: always SHA-256 */
    uint8_t initial_secret[32];
    if (hkdf_extract_digest("SHA-256",
                             QUIC_V1_SALT, sizeof(QUIC_V1_SALT),
                             dcid, dcid_len, initial_secret, 32) < 0) {
        LOG_ERROR("[quic-crypto] hkdf_extract initial_secret failed");
        g_suite = saved_suite;
        return -1;
    }
    hex_dump("derive_initial: initial_secret", initial_secret, 32);

    /* client_initial_secret = HKDF-Expand-Label(initial_secret, "client in", "", 32) */
    uint8_t client_secret[32];
    if (hkdf_expand_label_digest("SHA-256",
                                  initial_secret, 32, "client in",
                                  NULL, 0, client_secret, 32) < 0) {
        LOG_ERROR("[quic-crypto] client in expand failed");
        g_suite = saved_suite;
        return -1;
    }

    /* server_initial_secret = HKDF-Expand-Label(initial_secret, "server in", "", 32) */
    uint8_t server_secret[32];
    if (hkdf_expand_label_digest("SHA-256",
                                  initial_secret, 32, "server in",
                                  NULL, 0, server_secret, 32) < 0) {
        LOG_ERROR("[quic-crypto] server in expand failed");
        g_suite = saved_suite;
        return -1;
    }

    LOG_DEBUG("[quic-crypto] --- client_write keys (server reads with these) ---");
    if (derive_quic_keys(client_write, client_secret, 32) < 0) { g_suite = saved_suite; return -1; }
    LOG_DEBUG("[quic-crypto] --- server_write keys ---");
    if (derive_quic_keys(server_write, server_secret, 32) < 0) { g_suite = saved_suite; return -1; }

    g_suite = saved_suite;
    LOG_DEBUG("[quic-crypto] derived initial keys (client+server)");
    return 0;
}

int quic_crypto_derive_from_secret(QuicCipherKeys *keys,
                                   const uint8_t *secret, size_t secret_len) {
    if (derive_quic_keys(keys, secret, secret_len) < 0) {
        LOG_ERROR("[quic-crypto] derive from secret failed");
        return -1;
    }
    LOG_DEBUG("[quic-crypto] derived keys from TLS secret (len=%zu)", secret_len);
    return 0;
}

/* ============================================
 * Key Update (RFC 9001 §6)
 * ============================================ */

int quic_crypto_derive_key_update(QuicCipherKeys *keys,
                                   const uint8_t *current_secret, size_t secret_len,
                                   uint8_t *new_secret_out, size_t new_secret_cap) {
    if (new_secret_cap < 32) return -1;

    /* updated_secret = HKDF-Expand-Label(current_secret, "quic ku", "", 32) */
    uint8_t updated_secret[32];
    if (hkdf_expand_label_digest(get_digest_name(keys->suite),
                                  current_secret, secret_len, "quic ku",
                                  NULL, 0, updated_secret, 32) < 0) {
        LOG_ERROR("[quic-crypto] key update HKDF-Expand-Label failed");
        return -1;
    }

    memcpy(new_secret_out, updated_secret, 32);

    if (derive_quic_keys(keys, updated_secret, 32) < 0) {
        LOG_ERROR("[quic-crypto] key update derive_quic_keys failed");
        return -1;
    }

    LOG_DEBUG("[quic-crypto] key update: derived next-generation keys");
    return 0;
}

/* ============================================
 * Key Update — 仅 AEAD (RFC 9001 §6)
 *
 * quic-go 的 rollKeys() 不轮转 header protector，
 * HP key 保持初始值不变。此函数只更新 AEAD key+iv。
 * ============================================ */

int quic_crypto_derive_key_update_aead(QuicCipherKeys *keys,
                                        const uint8_t *current_secret, size_t secret_len,
                                        uint8_t *new_secret_out, size_t new_secret_cap) {
    if (new_secret_cap < 32) return -1;

    uint8_t updated_secret[32];
    if (hkdf_expand_label_digest(get_digest_name(keys->suite),
                                  current_secret, secret_len, "quic ku",
                                  NULL, 0, updated_secret, 32) < 0) {
        LOG_ERROR("[quic-crypto] key update aead HKDF-Expand-Label failed");
        return -1;
    }

    memcpy(new_secret_out, updated_secret, 32);

    /* 只推导 quic key 和 quic iv，保留原有 hp_key */
    int klen = g_key_len[g_suite];
    keys->key_len = klen;
    keys->suite   = g_suite;
    uint8_t ok = (uint8_t)(hkdf_expand_label_digest(get_digest_name(keys->suite),
                                                      updated_secret, 32,
                                                      "quic key",
                                                      NULL, 0,
                                                      keys->key, klen) == 0);
    ok &= (uint8_t)(hkdf_expand_label_digest(get_digest_name(keys->suite),
                                             updated_secret, 32, "quic iv",
                                             NULL, 0, keys->iv, 12) == 0);
    keys->initialized = ok ? 1 : 0;

    LOG_DEBUG("[quic-crypto] key update aead: %d-byte keys (hp unchanged)", klen);
    return ok ? 0 : -1;
}

/* ============================================
 * AEAD 加解密 (RFC 9001 §5.3)
 * ============================================ */

static void compute_nonce(const uint8_t iv[12], uint64_t pn, uint8_t nonce[12]) {
    memcpy(nonce, iv, 12);
    /* XOR packet number (big-endian) into last 8 bytes of nonce */
    for (int i = 0; i < 8; i++) {
        nonce[11 - i] ^= (uint8_t)(pn >> (8 * i));
    }
}

int quic_crypto_encrypt(QuicCipherKeys *keys, uint64_t pn,
                        const uint8_t *aad, size_t aad_len,
                        uint8_t *plaintext, size_t plaintext_len,
                        uint8_t *out, size_t *out_len) {
    if (!keys->initialized) return -1;

    uint8_t nonce[12];
    compute_nonce(keys->iv, pn, nonce);

    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return -1;

    int ret = -1;
    int outl = 0, finl = 0;

    if (EVP_EncryptInit_ex2(ctx, g_aead[keys->suite], keys->key, nonce, NULL) != 1)
        goto done;

    /* AAD */
    if (EVP_EncryptUpdate(ctx, NULL, &outl, aad, (int)aad_len) != 1)
        goto done;

    /* plaintext */
    if (EVP_EncryptUpdate(ctx, out, &outl, plaintext, (int)plaintext_len) != 1)
        goto done;

    /* finalize */
    if (EVP_EncryptFinal_ex(ctx, out + outl, &finl) != 1)
        goto done;
    outl += finl;

    /* GCM tag must be retrieved explicitly and appended */
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_GET_TAG, 16, out + outl) != 1)
        goto done;

    *out_len = (size_t)(outl + 16);
    ret = 0;

done:
    EVP_CIPHER_CTX_free(ctx);
    if (ret < 0) LOG_ERROR("[quic-crypto] AEAD encrypt failed");
    return ret;
}

int quic_crypto_decrypt(QuicCipherKeys *keys, uint64_t pn,
                        const uint8_t *aad, size_t aad_len,
                        const uint8_t *in, size_t in_len,
                        uint8_t *out, size_t *out_len) {
    if (!keys->initialized) return -1;
    if (in_len < 16) return -1; /* need at least the tag */

    uint8_t nonce[12];
    compute_nonce(keys->iv, pn, nonce);

    LOG_DEBUG("[quic-crypto] decrypt: pn=%llu", (unsigned long long)pn);
    hex_dump("decrypt: key", keys->key, 16);
    hex_dump("decrypt: iv", keys->iv, 12);
    hex_dump("decrypt: nonce", nonce, 12);
    hex_dump("decrypt: aad", aad, aad_len < 64 ? aad_len : 64);
    hex_dump("decrypt: ct(including tag)", in, in_len < 64 ? in_len : 64);

    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return -1;

    int ret = -1;
    int outl = 0, finl = 0;

    if (EVP_DecryptInit_ex2(ctx, g_aead[keys->suite], keys->key, nonce, NULL) != 1)
        goto done;

    /* AAD */
    if (EVP_DecryptUpdate(ctx, NULL, &outl, aad, (int)aad_len) != 1)
        goto done;

    /* ciphertext (without tag) */
    size_t ct_only = in_len - 16;
    if (EVP_DecryptUpdate(ctx, out, &outl, in, (int)ct_only) != 1)
        goto done;

    /* set expected tag before finalize */
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_TAG, 16, (void*)(in + ct_only)) != 1)
        goto done;

    /* finalize — verifies tag */
    if (EVP_DecryptFinal_ex(ctx, out + outl, &finl) != 1)
        goto done;

    *out_len = (size_t)(outl + finl);
    ret = 0;
    LOG_DEBUG("[quic-crypto] decrypt ok: pn=%llu plaintext[%zu]",
              (unsigned long long)pn, *out_len);
    hex_dump("decrypt: pt", out, *out_len < 128 ? *out_len : 128);

done:
    EVP_CIPHER_CTX_free(ctx);
    if (ret < 0) LOG_DEBUG("[quic-crypto] AEAD decrypt/verify failed");
    return ret;
}

/* ============================================
 * Header Protection (RFC 9001 §5.4)
 * ============================================ */

static void hp_mask(QuicCipherKeys *keys,
                    const uint8_t *sample, uint8_t mask[16]) {
    if (!g_hp[keys->suite]) return;

    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return;

    int outl = 0;
    if (EVP_EncryptInit_ex2(ctx, g_hp[keys->suite], keys->hp_key, NULL, NULL) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return;
    }
    EVP_CIPHER_CTX_set_padding(ctx, 0);
    EVP_EncryptUpdate(ctx, mask, &outl, sample, 16);
    EVP_CIPHER_CTX_free(ctx);
}

void quic_crypto_hp_apply(QuicCipherKeys *keys,
                          uint8_t *pkt, size_t pkt_len,
                          size_t pn_offset, size_t pn_len) {
    if (!keys->initialized) return;

    /* sample 从 pn_offset + 4 开始取 16 字节（不依赖 pn_len） */
    size_t sample_off = pn_offset + 4;
    if (sample_off + 16 > pkt_len) return;

    uint8_t mask[16];
    hp_mask(keys, pkt + sample_off, mask);

    /* 长头/短头判断：第一字节 bit7 */
    if (pkt[0] & 0x80) {
        /* 长头：遮挡低 4 位 */
        pkt[0] ^= (mask[0] & 0x0f);
    } else {
        /* 短头：遮挡低 5 位 */
        pkt[0] ^= (mask[0] & 0x1f);
    }

    /* 遮挡 PN 字段 */
    for (size_t i = 0; i < pn_len; i++) {
        pkt[pn_offset + i] ^= mask[1 + i];
    }
}

int quic_crypto_hp_remove_long(QuicCipherKeys *keys,
                                uint8_t *pkt, size_t pkt_len,
                                size_t pn_offset) {
    if (!keys->initialized) return 0;

    size_t sample_off = pn_offset + 4;
    if (sample_off + 16 > pkt_len) return 0;

    LOG_DEBUG("[quic-crypto] hp_remove_long: pn_offset=%zu sample_off=%zu pkt_len=%zu",
              pn_offset, sample_off, pkt_len);
    hex_dump("hp_remove: sample", pkt + sample_off, 16);

    uint8_t mask[16];
    hp_mask(keys, pkt + sample_off, mask);
    hex_dump("hp_remove: mask", mask, 16);

    uint8_t first_byte_before = pkt[0];
    pkt[0] ^= (mask[0] & 0x0f);

    size_t pn_len = (size_t)((pkt[0] & 0x03) + 1);
    LOG_DEBUG("[quic-crypto] hp_remove_long: first_byte %02x->%02x pn_len=%zu",
              first_byte_before, pkt[0], pn_len);

    for (size_t i = 0; i < pn_len; i++) {
        pkt[pn_offset + i] ^= mask[1 + i];
    }

    /* decode PN for logging */
    uint64_t decoded_pn = 0;
    for (size_t i = 0; i < pn_len; i++) {
        decoded_pn = (decoded_pn << 8) | pkt[pn_offset + i];
    }
    LOG_DEBUG("[quic-crypto] hp_remove_long: decoded_pn=%llu", (unsigned long long)decoded_pn);

    return (int)pn_len;
}

int quic_crypto_hp_remove_short(QuicCipherKeys *keys,
                                 uint8_t *pkt, size_t pkt_len,
                                 size_t pn_offset) {
    if (!keys->initialized) return 0;

    size_t sample_off = pn_offset + 4;
    if (sample_off + 16 > pkt_len) return 0;

    uint8_t mask[16];
    hp_mask(keys, pkt + sample_off, mask);

    /* 短头：还原第一字节的低 5 位 */
    pkt[0] ^= (mask[0] & 0x1f);

    size_t pn_len = (size_t)((pkt[0] & 0x03) + 1);

    for (size_t i = 0; i < pn_len; i++) {
        pkt[pn_offset + i] ^= mask[1 + i];
    }

    return (int)pn_len;
}

/* ============================================
 * Retry 完整性标签 (RFC 9001 §5.8)
 * ============================================ */

int quic_crypto_compute_retry_tag(const uint8_t *pseudo_pkt, size_t pseudo_len,
                                   uint8_t tag_out[16]) {
    uint8_t retry_key[16], retry_nonce[12];
    if (hkdf_expand_label_digest("SHA-256", QUIC_V1_SALT, 20, "retry key",
                                  NULL, 0, retry_key, 16) < 0)
        return -1;
    if (hkdf_expand_label_digest("SHA-256", QUIC_V1_SALT, 20, "retry nonce",
                                  NULL, 0, retry_nonce, 12) < 0)
        return -1;

    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return -1;

    int ret = -1, outl = 0;
    if (EVP_EncryptInit_ex2(ctx, g_aead[0], retry_key, retry_nonce, NULL) != 1)
        goto done;
    if (EVP_EncryptUpdate(ctx, NULL, &outl, pseudo_pkt, (int)pseudo_len) != 1)
        goto done;
    if (EVP_EncryptFinal_ex(ctx, NULL, &outl) != 1)
        goto done;
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_GET_TAG, 16, tag_out) != 1)
        goto done;
    ret = 0;
done:
    EVP_CIPHER_CTX_free(ctx);
    return ret;
}
