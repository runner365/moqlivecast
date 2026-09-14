#include "rtmp_handshake.hpp"
#include "rtmp_pub.hpp"
#include "utils/logger.hpp"
#include "utils/byte_stream.hpp"
#include "utils/timeex.hpp"

namespace cpp_streamer
{

void RtmpRandomGenerate(uint8_t* bytes, int size) {   
    for (int i = 0; i < size; i++) {
        // the common value in [0x0f, 0xf0]
        bytes[i] = 0x0f + (rand() % (256 - 0x0f - 0x0f));
    }
}

int HmacSha256(const char* key, int key_size, const char* data, int data_size, char* digest)
{   
    EVP_MAC* mac = EVP_MAC_fetch(NULL, "HMAC", NULL);
    if (!mac) {
        return -1;
    }

    EVP_MAC_CTX* ctx = EVP_MAC_CTX_new(mac);
    EVP_MAC_free(mac);
    
    if (!ctx) {
        return -1;
    }

    OSSL_PARAM params[2];
    params[0] = OSSL_PARAM_construct_utf8_string("digest", (char*)"SHA256", 0);
    params[1] = OSSL_PARAM_construct_end();
    
    if (EVP_MAC_init(ctx, (const unsigned char*)key, key_size, params) != 1) {
        EVP_MAC_CTX_free(ctx);
        return -1;
    }

    if (EVP_MAC_update(ctx, (const unsigned char*)data, data_size) != 1) {
        EVP_MAC_CTX_free(ctx);
        return -1;
    }

    size_t digest_size = 0;
    if (EVP_MAC_final(ctx, (unsigned char*)digest, &digest_size, HASH_SIZE) != 1) {
        EVP_MAC_CTX_free(ctx);
        return -1;
    }

    EVP_MAC_CTX_free(ctx);

    if (digest_size != 32) {
        return -1;
    }
    
    return 0;
}

uint32_t CalcValidDigestOffset(uint32_t offset) {
    const int MAX_OFFSET = 764 - 32 - 4;
    
    uint32_t valid_offset = 0;
    uint8_t* p = (uint8_t*)&offset;
    valid_offset += *p++;
    valid_offset += *p++;
    valid_offset += *p++;
    valid_offset += *p++;
    
    return valid_offset % MAX_OFFSET;
}

uint32_t CalcValidKeyOffset(uint32_t offset) {
    const int MAX_OFFSET = 764 - 128 - 4;
    
    uint32_t valid_offset = 0;
    uint8_t* p = (uint8_t*)&offset;
    valid_offset += *p++;
    valid_offset += *p++;
    valid_offset += *p++;
    valid_offset += *p++;
    
    return valid_offset % MAX_OFFSET;
}

C1S1Handle::C1S1Handle(Logger* logger):logger_(logger) {
    digest_random0_ = nullptr;
    digest_random1_ = nullptr;
    digest_random0_size_ = 0;
    digest_random1_size_ = 0;
}

C1S1Handle::~C1S1Handle() {
    if (key_random0_) {
        delete[] key_random0_;
    }
    if (key_random1_) {
        delete[] key_random1_;
    }
    if (digest_random0_) {
        delete[] digest_random0_;
        digest_random0_ = nullptr;
    }
    if (digest_random1_) {
        delete[] digest_random1_;
        digest_random1_ = nullptr;
    }
}

int C1S1Handle::ParseC1(char* c1, size_t len) {
    int ret = 0;
    uint8_t* p = (uint8_t*)c1;

    memcpy(c1_data_, c1, sizeof(c1_data_));

    c1_time_ = ByteStream::Read4Bytes(p);
    p += 4;
    c1_version_ = ByteStream::Read4Bytes(p);
    p += 4;

    //schema1 first which make ffmpeg happy
    ret = TrySchema1(p);
    if (ret != 0) {
        return ret;
    }

    bool is_valid = CheckDigestValid(SCHEMA1);
    if (!is_valid) {

        ret = TrySchema0(p);
        if (ret != 0) {
            return ret;
        }
        is_valid = CheckDigestValid(SCHEMA0);
        if (!is_valid) {
            LogErrorf(logger_, "try to rtmp handshake in simple mode");
            schema_ = SCHEMA0;
            return RTMP_SIMPLE_HANDSHAKE;
        }
        schema_ = SCHEMA0;
    } else {
        schema_ = SCHEMA1;
    }

    return ret;
}

int C1S1Handle::ParseS0S1S2(char* s0s1s2_data, size_t len) {
    uint8_t* p = (uint8_t*)s0s1s2_data;

    if (p[0] != RTMP_HANDSHAKE_VERSION) {
        LogErrorf(logger_, "s0s1s3 version error:0x%02x", *p);
        return -1;
    }
    p++;
    s1_time_sec_ = ByteStream::Read4Bytes(p);
    p += 4;
    s1_version_  = ByteStream::Read4Bytes(p);
    return 0;
}

int C1S1Handle::MakeC2(char* c2_data) {
    int ret = 0;
    uint8_t* p = (uint8_t*)c2_data;

    RtmpRandomGenerate(p, 1536);
    uint32_t now_ms = (uint32_t)now_millisec();
    ByteStream::Write4Bytes(p, now_ms);
    p += 4;
    ByteStream::Write4Bytes(p, s1_time_sec_);
    return ret;
}

int C1S1Handle::MakeC0C1(char* c0c1_data) {
    int ret = 0;
    char* c1_digest = nullptr;
    uint8_t* p = (uint8_t*)c0c1_data;

    ret = MakeC1Scheme1Digest(c1_digest);
    if ((ret < 0) || (c1_digest == nullptr)) {
        return -1;
    }

    //c0
    p[0] = RTMP_HANDSHAKE_VERSION;//0x03
    p++;

    //write time and version
    ByteStream::Write4Bytes(p, c1_time_);
    p += 4;
    ByteStream::Write4Bytes(p, c1_version_);
    p += 4;

    //write digest
    ByteStream::Write4Bytes(p, c1_digest_offset_);
    p += 4;

    if ((digest_random0_ == nullptr) || (digest_random0_size_ <= 0)) {
        delete[] c1_digest;
        return -1;
    }
    memcpy(p, digest_random0_, digest_random0_size_);
    p += digest_random0_size_;

    memcpy(p, c1_digest, 32);
    p += 32;
    delete[] c1_digest;

    if ((digest_random1_ == nullptr) || (digest_random1_size_ <= 0)) {
        return -1;
    }
    memcpy(p, digest_random1_, digest_random1_size_);
    p += digest_random1_size_;

    //write key
    memcpy(p, key_random0_, key_random0_size_);
    p += key_random0_size_;

    memcpy(p, c1_key_data_, sizeof(c1_key_data_));
    p += sizeof(c1_key_data_);

    memcpy(p, key_random1_, key_random1_size_);
    p += key_random1_size_;

    ByteStream::Write4Bytes(p, c1_key_offset_);
    p += 4;

    assert(p == ((uint8_t*)c0c1_data + 1 + 1536));


    return ret;
}

int C1S1Handle::MakeS1(char* s1_data) {
    int ret;
    s1_time_sec_ = GetC1Time();//use c1 time
    s1_version_  = 0x01000504;

    DHGenKey dh(logger_);
    ret = dh.Init(true);
    if (ret != 0) {
        LogErrorf(logger_, "dh Init error...");
        return ret;
    }

    uint32_t key_size = 128;
    ret = dh.CopySharedKey(c1_key_data_, sizeof(c1_key_data_), s1_key_data_, key_size);
    if (ret != 0) {
        LogErrorf(logger_, "dh copy shared key error...");
        return ret;
    }
    char* s1_digest = nullptr;
    ret = MakeS1Digest(s1_digest);
    if (ret != 0) {
        LogErrorf(logger_, "make s1 digest error:%d", ret);
        return ret;
    }
    memcpy(s1_digest_data_, s1_digest, sizeof(s1_digest_data_));
    delete[] s1_digest;

    uint8_t* p = (uint8_t*)s1_data;
    ByteStream::Write4Bytes(p, s1_time_sec_);
    p += 4;
    ByteStream::Write4Bytes(p, s1_version_);
    p += 4;

    if (schema_ == SCHEMA0) {
        MakeSchema0(p);
    } else if (schema_ == SCHEMA1) {
        MakeSchema1(p);
    } else {
        LogErrorf(logger_, "schema not init:%d", (int)schema_);
        return -1;
    }
    return 0;
}


char* C1S1Handle::GetC1Digest() {
    return digest_data_;
}

uint32_t C1S1Handle::GetC1Time() {
    return c1_time_;
}

char* C1S1Handle::GetC1Data() {
    return c1_data_;
}

int C1S1Handle::ParseKey(uint8_t* data) {
    uint8_t* p = data;
    //key: 764bytes
    //---- random-data: (offset)bytes
    //---- key-data: 128bytes
    //---- random-data: (764-offset-128-4)bytes
    //---- offset: 4bytes

    uint8_t* offset_p = p + 764 - sizeof(uint32_t);
    c1_key_offset_ = ByteStream::Read4Bytes(offset_p);

    uint32_t real_offset = CalcValidKeyOffset(c1_key_offset_);
    assert(real_offset < (764 - sizeof(uint32_t)));
    key_random0_size_ = real_offset;
    assert(key_random0_size_ < (764 - sizeof(uint32_t)));
    if (key_random0_) {
        delete[] key_random0_;
    }
    key_random0_ = new char[key_random0_size_];
    memcpy(key_random0_, p, key_random0_size_);
    p += key_random0_size_;

    memcpy(c1_key_data_, p, sizeof(c1_key_data_));//128bytes key
    p += sizeof(c1_key_data_);

    key_random1_size_ = 764 - real_offset - 128 - 4;
    assert(key_random1_size_ < (764 - sizeof(uint32_t)));
    if (key_random1_) {
        delete[] key_random1_;
    }
    key_random1_ = new char[key_random1_size_];
    memcpy(key_random1_, p, key_random1_size_);
    p += key_random1_size_ + 4;

    assert(p == (data + 764));

    return 0;
}

int C1S1Handle::ParseDigest(uint8_t* data) {
    uint8_t* p = data;

    //digest: 764bytes
    //---- offset: 4bytes
    //---- random-data: (offset)bytes
    //---- digest-data: 32bytes
    //---- random-data: (764-4-offset-32)bytes
    c1_digest_offset_ = ByteStream::Read4Bytes(p);
    p += 4;

    uint32_t real_offset = CalcValidDigestOffset(c1_digest_offset_);
    digest_random0_size_ = real_offset;
    assert(digest_random0_size_ < (764 - sizeof(uint32_t)));
    if (digest_random0_) {
        delete[] digest_random0_;
    }
    digest_random0_ = new char[digest_random0_size_];
    memcpy(digest_random0_, p, digest_random0_size_);
    p += digest_random0_size_;

    memcpy(digest_data_, p, sizeof(digest_data_));
    p += sizeof(digest_data_);

    digest_random1_size_ = 764 - 4 - real_offset - 32;
    assert(digest_random1_size_ < (764 - sizeof(uint32_t)));
    if (digest_random1_) {
        delete[] digest_random1_;
    }
    digest_random1_ = new char[digest_random1_size_];
    memcpy(digest_random1_, p, digest_random1_size_);

    assert((p + digest_random1_size_) == (data + 764));
    return 0;
}

bool C1S1Handle::CheckDigestValid(enum HANDSHAKE_SCHEMA schema) {
    size_t first_part_len = 0;
    size_t second_part_len = 0;
    char c1_digest[32];
    size_t digest_len = 32;
    char input_data[1536-32];

    HmacSha256Handler hmac;

    int ret = hmac.Init(GENUINE_FLASH_PLAYER_KEY, 30);
    if (ret != 0) {
        LogErrorf(logger_, "hmac Init error.");
        return ret;
    }

    if (schema == SCHEMA0) {
        // time + ver + key part(764) + digest offset(4bytes) + rand0 part
        first_part_len  = 8 + 764 + 4 + digest_random0_size_;
        second_part_len = sizeof(c1_data_) - first_part_len - 32;
        memcpy(input_data, c1_data_, first_part_len);
        memcpy(input_data + first_part_len, c1_data_ + first_part_len + 32, second_part_len);
    } else if (schema == SCHEMA1) {
        // time + ver + digest offset(4bytes) + rand0 part
        first_part_len  = 8 + 4 + digest_random0_size_;
        second_part_len = sizeof(c1_data_) - first_part_len - 32;
        memcpy(input_data, c1_data_, first_part_len);
        memcpy(input_data + first_part_len, c1_data_ + first_part_len + 32, second_part_len);
    } else {
        LogErrorf(logger_, "unkown handshake schema:%d", (int)schema);
        return false;
    }
    assert(first_part_len + second_part_len == sizeof(input_data));

    ret = hmac.Update((uint8_t*)input_data, sizeof(input_data));
    if (ret != 0) {
        LogErrorf(logger_, "hmac Update error.");
        return ret;
    }
    /*
    ret = hmac.Update((uint8_t*)c1_data_ + first_part_len + 32, second_part_len);
    if (ret != 0) {
        LogErrorf(logger_, "hmac Update error.");
        return ret;
    }
    */
    ret = hmac.GetFinal((uint8_t*)c1_digest, digest_len);
    if (ret != 0) {
        LogErrorf(logger_, "hmac final error.");
        return ret;
    }
    assert(digest_len == 32);

    bool is_equal = ByteStream::BytesIsEqual(digest_data_, c1_digest, sizeof(digest_data_));

    if (!is_equal) {
        LogErrorf(logger_, "the digest data is not equal, schema:%d", schema);
        //log_info_data((uint8_t*)digest_data_, sizeof(digest_data_), "digest_data");
        //log_info_data((uint8_t*)c1_digest, sizeof(digest_data_), "make c1 digest data");
    }
    return is_equal;
}

int C1S1Handle::TrySchema0(uint8_t* data) {
    uint8_t* p = data;

    //schema0: key first + digest second
    ParseKey(p);
    p += 764;
    ParseDigest(p);

    return 0;
}

int C1S1Handle::TrySchema1(uint8_t* data) {
    uint8_t* p = data;

    //schema1: digest first + key second
    ParseDigest(p);
    p += 764;
    ParseKey(p);

    return 0;
}

void C1S1Handle::PrepareDigest() {
    c1_digest_offset_ = rand();

    uint32_t real_offset = CalcValidDigestOffset(c1_digest_offset_);
    digest_random0_size_ = real_offset;
    if (digest_random0_) {
        delete[] digest_random0_;
    }
    digest_random0_ = new char[digest_random0_size_];
    RtmpRandomGenerate((uint8_t*)digest_random0_, digest_random0_size_);

    RtmpRandomGenerate((uint8_t*)digest_data_, sizeof(digest_data_));

    digest_random1_size_ = 764 - 4 - real_offset - 32;
    if (digest_random1_) {
        delete[] digest_random1_;
    }
    digest_random1_ = new char[digest_random1_size_];
    RtmpRandomGenerate((uint8_t*)digest_random1_, digest_random1_size_);

    return;
}

void C1S1Handle::PrepareKey() {
    c1_key_offset_ = rand();

    uint32_t real_offset = CalcValidKeyOffset(c1_key_offset_);
    key_random0_size_ = real_offset;

    if (key_random0_) {
        delete[] key_random0_;
    }
    key_random0_ = new char[key_random0_size_];
    RtmpRandomGenerate((uint8_t*)key_random0_, key_random0_size_);

    RtmpRandomGenerate((uint8_t*)c1_key_data_, sizeof(c1_key_data_));

    key_random1_size_ = 764 - real_offset - 128 - 4;
    if (key_random1_) {
        delete[] key_random1_;
    }
    key_random1_ = new char[key_random1_size_];
    RtmpRandomGenerate((uint8_t*)key_random1_, key_random1_size_);
}

int C1S1Handle::MakeC1Scheme1Digest(char*& c1_digest) {
    PrepareDigest();
    PrepareKey();

    c1_time_    = (uint32_t)now_millisec();
    c1_version_ = 0x80000702; // client c1 version

    /*
     * c1s1 is splited by digest:
     *     c1s1-part1: n bytes (time, version, key and digest-part1).
     *     digest-data: 32bytes
     *     c1s1-part2: (1536-n-32)bytes (digest-part2)
     */
    char* c1s1_joined_data = new char[1536 -32];
    uint8_t* p = (uint8_t*)c1s1_joined_data;

    /* ++++++ c1s1-part1: (time, version, key and digest-part1)++++++ */
    ByteStream::Write4Bytes(p, c1_time_);
    p += 4;
    ByteStream::Write4Bytes(p, c1_version_);
    p += 4;

    /* +++++ digest-part1 +++++ */
    ByteStream::Write4Bytes(p, c1_digest_offset_);
    p += 4;

    memcpy(p, digest_random0_, digest_random0_size_);
    p += digest_random0_size_;

    //skip digest-data: 32bytes

    /* +++++ digest-part2 +++++ */
    memcpy(p, digest_random1_, digest_random1_size_);
    p += digest_random1_size_;

    /* +++++ key ++++ */
    memcpy(p, key_random0_, key_random0_size_);
    p += key_random0_size_;

    memcpy(p, c1_key_data_, sizeof(c1_key_data_));
    p += sizeof(c1_key_data_);

    memcpy(p, key_random1_, key_random1_size_);
    p += key_random1_size_;

    ByteStream::Write4Bytes(p, c1_key_offset_);
    p += 4;

    assert(p == ((uint8_t*)c1s1_joined_data + 1536 -32));

    c1_digest = new char[HASH_SIZE];

    int ret = HmacSha256((const char*)GENUINE_FLASH_PLAYER_KEY, 30,
                    c1s1_joined_data, 1536 - 32, c1_digest);
    if (ret != 0) {
        delete[] c1s1_joined_data;
        return ret;
    }

    delete[] c1s1_joined_data;
    return ret;
}

int C1S1Handle::MakeS1Digest(char*& s1_digest) {
    const int JOINED_BYTES_SIZE = 1536 - 32;
    int ret = 0;
    char joined_bytes[JOINED_BYTES_SIZE];
    uint8_t* p = (uint8_t*)joined_bytes;

    ByteStream::Write4Bytes(p, s1_time_sec_);
    p += 4;
    ByteStream::Write4Bytes(p, s1_version_);
    p += 4;

    /* ++++++ key part ++++++ */
    memcpy(p, key_random0_, key_random0_size_);
    p += key_random0_size_;

    memcpy(p, c1_key_data_, sizeof(c1_key_data_));
    p += sizeof(c1_key_data_);

    memcpy(p, key_random1_, key_random1_size_);
    p += key_random1_size_;

    ByteStream::Write4Bytes(p, c1_key_offset_);
    p += 4;

    /* ++++++ digest part(no digest inside) ++++++ */
    ByteStream::Write4Bytes(p, c1_digest_offset_);
    p += 4;

    memcpy(p, digest_random0_, digest_random0_size_);
    p += digest_random0_size_;

    //no digest inside: less 32 bytes

    memcpy(p, digest_random1_, digest_random1_size_);
    p += digest_random1_size_;

    s1_digest = new char[HASH_SIZE];
    ret = HmacSha256((char*)GENUINE_FLASH_PLAYER_KEY, 36, joined_bytes, JOINED_BYTES_SIZE, s1_digest);
    if (ret != 0) {
        LogErrorf(logger_, "HmacSha256 error:%d", ret);
        return ret;
    }

    return ret;
}

int C1S1Handle::MakeKey(uint8_t* data) {
    uint8_t* p = data;
    //key: 764bytes
    //---- random-data: (offset)bytes
    //---- key-data: 128bytes
    //---- random-data: (764-offset-128-4)bytes
    //---- offset: 4bytes
    memcpy(p, key_random0_, key_random0_size_);
    p += key_random0_size_;

    memcpy(p, s1_key_data_, sizeof(s1_key_data_));
    p += sizeof(s1_key_data_);

    memcpy(p, key_random1_, key_random1_size_);
    p += key_random1_size_;

    ByteStream::Write4Bytes(p, c1_key_offset_);

    return 0;
}

int C1S1Handle::MakeDigest(uint8_t* data) {
    uint8_t* p = data;

    //digest: 764bytes
    //---- offset: 4bytes
    //---- random-data: (offset)bytes
    //---- digest-data: 32bytes
    //---- random-data: (764-4-offset-32)bytes
    ByteStream::Write4Bytes(p, c1_digest_offset_);
    p += 4;

    memcpy(p, digest_random0_, digest_random0_size_);
    p += digest_random0_size_;

    memcpy(p, s1_digest_data_, sizeof(s1_digest_data_));
    p += sizeof(s1_digest_data_);

    memcpy(p, digest_random1_, digest_random1_size_);
    p += digest_random1_size_;

    return 0;
}

int C1S1Handle::MakeSchema0(uint8_t* data) {
    uint8_t* p = data;

    //schema0: key first + digest second
    MakeKey(p);
    p += 764;
    MakeDigest(p);

    return 0;
}

int C1S1Handle::MakeSchema1(uint8_t* data) {
    uint8_t* p = data;

    //schema1: digest first + key second
    MakeDigest(p);
    p += 764;
    MakeKey(p);
    return 0;
}

RtmpServerHandshake::RtmpServerHandshake(Logger* logger): logger_(logger)
                                                        , c1s1_(logger)
                                                        , c2s2_(logger)
{
    c0_version_ = 0;
    memset(s1_body_, 0, sizeof(s1_body_));
    memset(s2_body_, 0, sizeof(s2_body_));
    LogInfof(logger_, "RtmpServerHandshake constructed.");
}

RtmpServerHandshake::~RtmpServerHandshake()
{
}

int RtmpServerHandshake::HandleC0C1(DataBuffer& recv_buffer) {
    const size_t c0_size = 1;
    const size_t c1_size = 1536;

    if (!recv_buffer.Require(c0_size + c1_size)) {
        return RTMP_NEED_READ_MORE;
    }
    return ParseC0C1(recv_buffer.Data());
}

int RtmpServerHandshake::HandleC2(DataBuffer& recv_buffer) {
    const size_t c2_size = 1536;

    if (!recv_buffer.Require(c2_size)) {
        return RTMP_NEED_READ_MORE;
    }
    //TODO_JOB: handle c2 data.
    recv_buffer.ConsumeData(c2_size);
    return RTMP_OK;
}

int RtmpServerHandshake::SendS0S1S2(std::vector<uint8_t>& s0s1s2) {
    const size_t s0s1s2_size = 3073; // 1 + 1536 + 1536

    s0s1s2.resize(s0s1s2_size);

    char* s1_data;
    int s1_len;
    char* s2_data;
    int s2_len;
    uint8_t* p = s0s1s2.data();

    /* ++++++ s0 ++++++*/
    RtmpRandomGenerate((uint8_t*)p, (int)s0s1s2.size());
    p[0] = RTMP_HANDSHAKE_VERSION;
    p++;

    /* ++++++ s1 ++++++*/
    s1_data = MakeS1Data(s1_len);
    if (!s1_data) {
        LogErrorf(logger_, "make s1 data error...");
        return -1;
    }
    assert(s1_len == 1536);
    memcpy(p, s1_data, s1_len);
    p += s1_len;

    /* ++++++ s2 ++++++*/
    s2_data = MakeS2Data(s2_len);
    if (!s2_data) {
        LogErrorf(logger_, "make s2 data error...");
        return -1;
    }
    assert(s2_len == 1536);
    memcpy(p, s2_data, s2_len);

    return RTMP_NEED_READ_MORE;
}

int RtmpServerHandshake::ParseC0C1(char* c0c1) {
    c0_version_ = c0c1[0];

    return c1s1_.ParseC1(c0c1 + 1, (size_t)1536);
}

char* RtmpServerHandshake::MakeS1Data(int& s1_len) {
    int ret = c1s1_.MakeS1((char*)s1_body_);

    if (ret != 0) {
        s1_len = 0;
        LogErrorf(logger_, "make s1 error:%d", ret);
        return nullptr;
    }
    s1_len = sizeof(s1_body_);
    return s1_body_;
}

char* RtmpServerHandshake::MakeS2Data(int& s2_len) {
    int ret;

    ret = c2s2_.CreateByDigest(c1s1_.GetC1Digest());
    if (ret != 0) {
        LogErrorf(logger_, "c2s2 create by digest s2 error...");
        return nullptr;
    }
    
    bool is_valid = c2s2_.ValidateS2(c1s1_.GetC1Digest());
    if (!is_valid) {
        LogErrorf(logger_, "c2s2 validate s2 error...");
        return nullptr;
    }
    c2s2_.Generate(s2_body_);
    s2_len = sizeof(s2_body_);
    return s2_body_;
}

}