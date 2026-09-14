#ifndef CO_RTMP_HANDSHAKE_HPP
#define CO_RTMP_HANDSHAKE_HPP
#ifdef _WIN64
#define WIN32_LEAN_AND_MEAN  // 屏蔽 Windows 旧版冗余头文件（包括 winsock.h）
#endif
#include "utils/logger.hpp"
#include "utils/data_buffer.hpp"
#include "utils/byte_stream.hpp"

#include <openssl/evp.h>
#include <openssl/param_build.h>
#include <openssl/core_names.h>

#include <vector>

namespace cpp_streamer
{
#define HASH_SIZE 512
#define RTMP_HANDSHAKE_VERSION 0x03

#define RFC2409_PRIME_1024 \
    "FFFFFFFFFFFFFFFFC90FDAA22168C234C4C6628B80DC1CD1" \
    "29024E088A67CC74020BBEA63B139B22514A08798E3404DD" \
    "EF9519B3CD3A431B302B0A6DF25F14374FE1356D6D51C245" \
    "E485B576625E7EC6F44C42E9A637ED6B0BFF5CB6F406B7ED" \
    "EE386BFB5A899FA5AE9F24117C4B1FE649286651ECE65381" \
    "FFFFFFFFFFFFFFFF"

enum HANDSHAKE_SCHEMA {
    SCHEMA_INIT = -1,
    SCHEMA0,
    SCHEMA1
};

// 62bytes Flash Player key which is used to sign the client packet.
static uint8_t GENUINE_FLASH_PLAYER_KEY[] = {
    0x47, 0x65, 0x6E, 0x75, 0x69, 0x6E, 0x65, 0x20,
    0x41, 0x64, 0x6F, 0x62, 0x65, 0x20, 0x46, 0x6C,
    0x61, 0x73, 0x68, 0x20, 0x50, 0x6C, 0x61, 0x79,
    0x65, 0x72, 0x20, 0x30, 0x30, 0x31,
    // "Genuine Adobe Flash Player 001"
    0xF0, 0xEE, 0xC2, 0x4A, 0x80, 0x68, 0xBE, 0xE8,
    0x2E, 0x00, 0xD0, 0xD1, 0x02, 0x9E, 0x7E, 0x57,
    0x6E, 0xEC, 0x5D, 0x2D, 0x29, 0x80, 0x6F, 0xAB,
    0x93, 0xB8, 0xE6, 0x36, 0xCF, 0xEB, 0x31, 0xAE
};//SIZE = 62

void RtmpRandomGenerate(uint8_t* bytes, int size);

uint32_t CalcValidDigestOffset(uint32_t offset);
uint32_t CalcValidKeyOffset(uint32_t offset);

int HmacSha256(const char* key, int key_size, const char* data, int data_size, char* digest);

class HmacSha256Handler
{
public:
    HmacSha256Handler() {
        ctx_ = EVP_MAC_CTX_new(EVP_MAC_fetch(NULL, "HMAC", NULL));
    }
    ~HmacSha256Handler() {
        if (ctx_) {
            EVP_MAC_CTX_free(ctx_);
            ctx_ = nullptr;
        }
    }

public:
    int Init(uint8_t* key, int key_len) {
        OSSL_PARAM params[2];
        params[0] = OSSL_PARAM_construct_utf8_string("digest", (char*)"SHA256", 0);
        params[1] = OSSL_PARAM_construct_end();
        
        if (EVP_MAC_init(ctx_, key, key_len, params) != 1) {
            return -1;
        }
        return 0;
    }

    int Update(uint8_t* data, size_t data_len) {
        if (EVP_MAC_update(ctx_, data, data_len) != 1) {
            return -1;
        }
        return 0;
    }

    int GetFinal(uint8_t* digest, size_t& digest_size) {
        if (EVP_MAC_final(ctx_, digest, &digest_size, EVP_MAX_MD_SIZE) != 1) {
            return -1;
        }
        return 0;
    }

private:
    EVP_MAC_CTX* ctx_ = nullptr;
};

class DHGenKey
{
private:
    EVP_PKEY* pkey_ = nullptr;
    Logger* logger_ = nullptr;

public:
    DHGenKey(Logger* logger):logger_(logger)
    {
    }
    ~DHGenKey()
    {
        Close();
    }

public:
    int Init(bool public_128bytes_key = false) {
        int ret;

        while(true) {
            ret = DoInit();
            if (ret != 0) {
                return ret;
            }

            if (public_128bytes_key) {
                // 获取公钥并检查大小
                BIGNUM* pub_key = NULL;
                if (EVP_PKEY_get_bn_param(pkey_, OSSL_PKEY_PARAM_PUB_KEY, &pub_key) != 1) {
                    LogWarnf(logger_, "failed to get public key");
                    continue;
                }
                int32_t key_size = BN_num_bytes(pub_key);
                BN_free(pub_key);
                
                if (key_size != 128) {
                    LogWarnf(logger_, "regenerate 128 bytes key, current=%d bytes", key_size);
                    continue;
                }
                LogWarnf(logger_, "get right key size:%d", key_size);
            }
            break;
        }
        return 0;
    }

    int CopySharedKey(const char* ppkey, uint32_t ppkey_size, char* skey, uint32_t& skey_size) {
        BIGNUM* peer_pub_bn = NULL;
        EVP_PKEY* peer_pkey = NULL;
        EVP_PKEY_CTX* derive_ctx = NULL;
        EVP_PKEY_CTX* ctx = NULL;
        OSSL_PARAM_BLD* param_bld = NULL;
        OSSL_PARAM* params = NULL;
        BIGNUM* p = NULL;
        BIGNUM* g = NULL;
        int ret = -1;
        size_t key_size = 0;

        // 将对端公钥转换为 BIGNUM
        if ((peer_pub_bn = BN_bin2bn((const unsigned char*)ppkey, ppkey_size, NULL)) == NULL) {
            LogErrorf(logger_, "BN_bin2bn error");
            return -1;
        }

        // 构建对端的 DH 公钥
        param_bld = OSSL_PARAM_BLD_new();
        if (!param_bld) {
            LogErrorf(logger_, "OSSL_PARAM_BLD_new error");
            BN_free(peer_pub_bn);
            return -1;
        }

        // 从当前密钥获取 p 和 g 参数
        if (EVP_PKEY_get_bn_param(pkey_, OSSL_PKEY_PARAM_FFC_P, &p) != 1 ||
            EVP_PKEY_get_bn_param(pkey_, OSSL_PKEY_PARAM_FFC_G, &g) != 1) {
            LogErrorf(logger_, "EVP_PKEY_get_bn_param error");
            goto cleanup;
        }

        if (OSSL_PARAM_BLD_push_BN(param_bld, OSSL_PKEY_PARAM_FFC_P, p) != 1 ||
            OSSL_PARAM_BLD_push_BN(param_bld, OSSL_PKEY_PARAM_FFC_G, g) != 1 ||
            OSSL_PARAM_BLD_push_BN(param_bld, OSSL_PKEY_PARAM_PUB_KEY, peer_pub_bn) != 1) {
            LogErrorf(logger_, "OSSL_PARAM_BLD_push_BN error");
            goto cleanup;
        }

        params = OSSL_PARAM_BLD_to_param(param_bld);
        if (!params) {
            LogErrorf(logger_, "OSSL_PARAM_BLD_to_param error");
            goto cleanup;
        }

        // 从参数创建对端公钥
        ctx = EVP_PKEY_CTX_new_from_name(NULL, "DH", NULL);
        if (!ctx || EVP_PKEY_fromdata_init(ctx) != 1 ||
            EVP_PKEY_fromdata(ctx, &peer_pkey, EVP_PKEY_PUBLIC_KEY, params) != 1) {
            LogErrorf(logger_, "create peer public key error");
            goto cleanup;
        }

        // 计算共享密钥
        derive_ctx = EVP_PKEY_CTX_new(pkey_, NULL);
        if (!derive_ctx || EVP_PKEY_derive_init(derive_ctx) != 1) {
            LogErrorf(logger_, "EVP_PKEY_derive_init error");
            goto cleanup;
        }

        if (EVP_PKEY_derive_set_peer(derive_ctx, peer_pkey) != 1) {
            LogErrorf(logger_, "EVP_PKEY_derive_set_peer error");
            goto cleanup;
        }

        key_size = skey_size;
        if (EVP_PKEY_derive(derive_ctx, (unsigned char*)skey, &key_size) != 1) {
            LogErrorf(logger_, "EVP_PKEY_derive error");
            goto cleanup;
        }

        if (key_size < ppkey_size) {
            LogWarnf(logger_, "shared key size=%zu, ppk_size=%u", key_size, ppkey_size);
        }

        skey_size = (uint32_t)key_size;
        ret = 0;

    cleanup:
        if (peer_pub_bn) BN_free(peer_pub_bn);
        if (p) BN_free(p);
        if (g) BN_free(g);
        if (peer_pkey) EVP_PKEY_free(peer_pkey);
        if (ctx) EVP_PKEY_CTX_free(ctx);
        if (derive_ctx) EVP_PKEY_CTX_free(derive_ctx);
        if (param_bld) OSSL_PARAM_BLD_free(param_bld);
        if (params) OSSL_PARAM_free(params);
        
        return ret;
    }

private:
    int DoInit() {
        Close();

        // 创建 p 和 g
        BIGNUM* p = NULL;
        BIGNUM* g = NULL;
        if ((p = BN_new()) == NULL || (g = BN_new()) == NULL) {
            LogErrorf(logger_, "BN_new error");
            if (p) BN_free(p);
            if (g) BN_free(g);
            return -1;
        }

        if (!BN_hex2bn(&p, RFC2409_PRIME_1024)) {
            LogErrorf(logger_, "BN_hex2bn error");
            BN_free(p);
            BN_free(g);
            return -1;
        }

        if (!BN_set_word(g, 2)) {
            LogErrorf(logger_, "BN_set_word error");
            BN_free(p);
            BN_free(g);
            return -1;
        }

        // 创建参数构建器
        OSSL_PARAM_BLD* param_bld = OSSL_PARAM_BLD_new();
        if (!param_bld) {
            LogErrorf(logger_, "OSSL_PARAM_BLD_new error");
            BN_free(p);
            BN_free(g);
            return -1;
        }

        // 构建 DH 参数
        if (OSSL_PARAM_BLD_push_BN(param_bld, OSSL_PKEY_PARAM_FFC_P, p) != 1 ||
            OSSL_PARAM_BLD_push_BN(param_bld, OSSL_PKEY_PARAM_FFC_G, g) != 1) {
            LogErrorf(logger_, "OSSL_PARAM_BLD_push_BN error");
            OSSL_PARAM_BLD_free(param_bld);
            BN_free(p);
            BN_free(g);
            return -1;
        }

        OSSL_PARAM* params = OSSL_PARAM_BLD_to_param(param_bld);
        OSSL_PARAM_BLD_free(param_bld);
        BN_free(p);
        BN_free(g);

        if (!params) {
            LogErrorf(logger_, "OSSL_PARAM_BLD_to_param error");
            return -1;
        }

        // 创建 DH 参数密钥
        EVP_PKEY_CTX* param_ctx = EVP_PKEY_CTX_new_from_name(NULL, "DH", NULL);
        if (!param_ctx) {
            LogErrorf(logger_, "EVP_PKEY_CTX_new_from_name error");
            OSSL_PARAM_free(params);
            return -1;
        }

        EVP_PKEY* param_key = NULL;
        if (EVP_PKEY_fromdata_init(param_ctx) != 1 ||
            EVP_PKEY_fromdata(param_ctx, &param_key, EVP_PKEY_KEY_PARAMETERS, params) != 1) {
            LogErrorf(logger_, "EVP_PKEY_fromdata error");
            EVP_PKEY_CTX_free(param_ctx);
            OSSL_PARAM_free(params);
            return -1;
        }

        EVP_PKEY_CTX_free(param_ctx);
        OSSL_PARAM_free(params);

        // 从参数生成密钥对
        EVP_PKEY_CTX* keygen_ctx = EVP_PKEY_CTX_new(param_key, NULL);
        EVP_PKEY_free(param_key);

        if (!keygen_ctx || EVP_PKEY_keygen_init(keygen_ctx) != 1) {
            LogErrorf(logger_, "EVP_PKEY_keygen_init error");
            if (keygen_ctx) EVP_PKEY_CTX_free(keygen_ctx);
            return -1;
        }

        if (EVP_PKEY_keygen(keygen_ctx, &pkey_) != 1) {
            LogErrorf(logger_, "EVP_PKEY_keygen error");
            EVP_PKEY_CTX_free(keygen_ctx);
            return -1;
        }

        EVP_PKEY_CTX_free(keygen_ctx);
        return 0;
    }

    void Close() {
        if (pkey_) {
            EVP_PKEY_free(pkey_);
            pkey_ = NULL;
        }
    }
};

class C1S1Handle
{
private:
    Logger* logger_ = nullptr;

public:
    C1S1Handle(Logger* logger);
    ~C1S1Handle();

public:
    int ParseC1(char* c1, size_t len);
    int ParseS0S1S2(char* s0s1s2_data, size_t len);
    int MakeS1(char* s1_data);
    int MakeC0C1(char* c0c1_data);
    int MakeC2(char* c2_data);
    uint32_t GetC1Time();
    char* GetC1Digest();
    char* GetC1Data();

private:
    int ParseKey(uint8_t* data);
    int ParseDigest(uint8_t* data);
    bool CheckDigestValid(enum HANDSHAKE_SCHEMA schema);
    int TrySchema0(uint8_t* data);
    int TrySchema1(uint8_t* data);
    void PrepareDigest();
    void PrepareKey();
    int MakeC1Scheme1Digest(char*& c1_digest);
    int MakeS1Digest(char*& s1_digest);
    int MakeKey(uint8_t* data);
    int MakeDigest(uint8_t* data);
    int MakeSchema0(uint8_t* data);
    int MakeSchema1(uint8_t* data);

private:
    enum HANDSHAKE_SCHEMA schema_ = SCHEMA_INIT;
    char c1_data_[1536];
    uint32_t c1_time_;
    uint32_t c1_version_;

    char* key_random0_ = nullptr;
    char* key_random1_ = nullptr;
    uint32_t key_random0_size_;
    uint32_t key_random1_size_;

    uint32_t c1_key_offset_;
    char c1_key_data_[128];

    uint32_t c1_digest_offset_;
    char* digest_random0_ = nullptr;
    char* digest_random1_ = nullptr;
    uint32_t digest_random0_size_;
    uint32_t digest_random1_size_;
    char digest_data_[32];

private:
    uint32_t s1_time_sec_;
    uint32_t s1_version_;
    char s1_key_data_[128];
    char s1_digest_data_[32];
};


class C2S2Handle
{
private:
    Logger* logger_ = nullptr;

public:
    C2S2Handle(Logger* logger):logger_() {
        RtmpRandomGenerate((uint8_t*)random_, sizeof(random_));

        RtmpRandomGenerate((uint8_t*)digest_, 32);
    }
    ~C2S2Handle() {

    }

public:
    void Generate(char* body) {
        memcpy(body, random_, 1504);
        memcpy(body + 1504, digest_, 32);
    }

    void Parse(char* body)
    {   
        memcpy(random_, body, sizeof(random_));
        memcpy(digest_, body + 1504, sizeof(digest_));
        return;
    }

    int CreateByDigest(char* c1_digest) {
        int ret = 0;
        char temp_key[HASH_SIZE];

        ret = HmacSha256((char*)GENUINE_FLASH_PLAYER_KEY, 68, c1_digest, 32, temp_key);
        if (ret != 0) {
            LogErrorf(logger_, "hmac sha256 error:%d", ret);
            return ret;
        }

        char temp_digest[HASH_SIZE];
        ret = HmacSha256(temp_key, 32, random_, 1504, temp_digest);
        if (ret != 0) {
            LogErrorf(logger_, "hmac sha256 error:%d", ret);
            return ret;
        }
        memcpy(digest_, temp_digest, 32);

        return 0;
    }

    bool ValidateS2(char* c1_digest) {
        int ret;
        char temp_key[HASH_SIZE];

        ret = HmacSha256((char*)GENUINE_FLASH_PLAYER_KEY, 68, c1_digest, 32, temp_key);
        if (ret != 0) {
            LogErrorf(logger_, "hmac sha256 error:%d", ret);
            return ret;
        }

        char temp_digest[HASH_SIZE];
        ret = HmacSha256(temp_key, 32, random_, 1504, temp_digest);
        if (ret != 0) {
            LogErrorf(logger_, "hmac sha256 error:%d", ret);
            return ret;
        }

        bool is_equal = ByteStream::BytesIsEqual(digest_, temp_digest, sizeof(digest_));
    
        return is_equal;
    }
private:
    char random_[1504];
    char digest_[32];
};

class RtmpServerHandshake
{
public:
    RtmpServerHandshake(Logger* logger);
    ~RtmpServerHandshake();

public:
    int HandleC0C1(DataBuffer& recv_buffer);
    int HandleC2(DataBuffer& recv_buffer);
    int SendS0S1S2(std::vector<uint8_t>& s0s1s2);

private:
    int ParseC0C1(char* c0c1);
    char* MakeS1Data(int& s1_len);
    char* MakeS2Data(int& s2_len);

private:
    Logger* logger_ = nullptr;

private:
    uint8_t c0_version_;
    C1S1Handle c1s1_;
    C2S2Handle c2s2_;
    char s1_body_[1536];
    char s2_body_[1536];
};

}

#endif