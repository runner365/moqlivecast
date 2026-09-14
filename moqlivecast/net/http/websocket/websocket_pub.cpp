#include "websocket_pub.hpp"
#include "utils/base64.hpp"
#include <stdint.h>
#include <string>
#include <openssl/evp.h>

namespace cpp_streamer
{
std::string GenWebSocketHashcode(const std::string& key) {
    std::string sec_key = key;
    sec_key += "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned int hash_len = 0;

    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) {
        return "";
    }

    if (EVP_DigestInit_ex(ctx, EVP_sha1(), nullptr) != 1 ||
        EVP_DigestUpdate(ctx, sec_key.data(), sec_key.size()) != 1 ||
        EVP_DigestFinal_ex(ctx, hash, &hash_len) != 1) {
        EVP_MD_CTX_free(ctx);
        return "";
    }

    EVP_MD_CTX_free(ctx);
    return Base64Encode(hash, hash_len);
}

}