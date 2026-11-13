#include "flv_h265.hpp"

#include <cstdio>
#include <cstring>

namespace cpp_streamer {

namespace {

uint32_t rd32(const uint8_t *p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
           (uint32_t(p[2]) << 8) | p[3];
}

uint16_t rd16(const uint8_t *p) {
    return (uint16_t(p[0]) << 8) | p[1];
}

bool is_hvc1(uint32_t fourcc) {
    return fourcc == ((uint32_t('h') << 24) | (uint32_t('v') << 16) |
                      (uint32_t('c') << 8) | uint32_t('1'));
}

} /* namespace */

bool IsHevcDecoderConfig(const uint8_t *cfg, size_t len) {
    /* HEVCDecoderConfigurationRecord:
     *   0     configurationVersion (=1)
     *   1..12 profile/tier/level/constraint fields
     *   13..14 min_spatial_segmentation_idc
     *   15 parallelismType
     *   16 chromaFormat
     *   17 bitDepthLumaMinus8
     *   18 bitDepthChromaMinus8
     *   19..20 avgFrameRate
     *   21 constantFrameRate/numTemporalLayers/temporalIdNested/lengthSizeMinusOne
     *   22 numOfArrays
     *   23... arrays: [1B completeness|type][2B count][2B len + NAL data]...
     */
    if (!cfg || len < 23 || cfg[0] != 1) return false;
    const size_t num_arrays = cfg[22];
    if (num_arrays == 0) return false;
    size_t off = 23;
    for (size_t i = 0; i < num_arrays; i++) {
        if (off + 3 > len) return false;
        const uint16_t count = rd16(cfg + off + 1);
        off += 3;
        if (count == 0) return false;
        for (uint16_t j = 0; j < count; j++) {
            if (off + 2 > len) return false;
            const uint16_t n = rd16(cfg + off);
            off += 2;
            if (n == 0 || off + n > len) return false;
            off += n;
        }
    }
    return off == len;
}

std::string HevcCodecString(const uint8_t *hvcC, size_t len) {
    if (!hvcC || len < 13) return "hvc1.1.6.L93.B0";
    const unsigned profile_space = (hvcC[1] >> 6) & 0x03;
    const unsigned tier = (hvcC[1] >> 5) & 0x01;
    const unsigned profile = hvcC[1] & 0x1f;
    const uint32_t compat = rd32(hvcC + 2);
    const unsigned level = hvcC[12];
    static const char *kSpace[] = {"", "A", "B", "C"};
    char out[96];
    if (profile == 1 && profile_space == 0 && compat == 0x60000000u) {
        /* Main profile: canonical string used by Chrome/WebCodecs. */
        snprintf(out, sizeof(out), "hvc1.1.6.%c%u.B0", tier ? 'H' : 'L', level);
    } else {
        snprintf(out, sizeof(out), "hvc1.%s%u.%x.%c%u",
                 kSpace[profile_space], profile, compat, tier ? 'H' : 'L', level);
    }
    return out;
}

bool NormalizeH265FlvVideoBody(const uint8_t *body, size_t len,
                               std::vector<uint8_t> *out,
                               size_t *payload_off) {
    if (!body || !out || !payload_off || len < 5) return false;
    const bool enhanced = (body[0] & 0x80) != 0;
    if (enhanced) {
        const uint8_t packet_type = body[0] & 0x0f;
        if (!is_hvc1(rd32(body + 1))) return false;
        size_t off = 5;
        if (packet_type == 1) {
            if (len < 8) return false;
            off += 3;
        } else if (packet_type != 0 && packet_type != 3) {
            return false;
        }
        out->assign(body, body + len);
        *payload_off = off;
        return true;
    }

    const uint8_t codec = body[0] & 0x0f;
    if (codec != 0x0c) return false;
    const uint8_t avc_packet_type = body[1];
    if (avc_packet_type != 0 && avc_packet_type != 1) return false;
    const bool seq = (avc_packet_type == 0);
    const bool key = ((body[0] >> 4) == 1);
    int64_t cts = 0;
    if (!seq) {
        int32_t c = (int32_t(body[2]) << 16) | (int32_t(body[3]) << 8) | body[4];
        if (c & 0x00800000) c -= 0x01000000;
        cts = c;
    }
    *out = MakeH265ExVideoBody(seq, key, cts, body + 5, len - 5);
    *payload_off = seq ? 5 : 8;
    return true;
}

std::vector<uint8_t> MakeH265ExVideoBody(bool seq, bool key, int64_t cts_ms,
                                         const uint8_t *payload,
                                         size_t payload_len) {
    std::vector<uint8_t> out;
    out.reserve(8 + payload_len);
    const uint8_t frame = key || seq ? 1 : 2;
    const uint8_t packet = seq ? 0 : 1;
    out.push_back(uint8_t(0x80 | (frame << 4) | packet));
    out.push_back('h');
    out.push_back('v');
    out.push_back('c');
    out.push_back('1');
    if (!seq) {
        const uint32_t bits = uint32_t(cts_ms) & 0x00ffffffu;
        out.push_back(uint8_t((bits >> 16) & 0xff));
        out.push_back(uint8_t((bits >> 8) & 0xff));
        out.push_back(uint8_t(bits & 0xff));
    }
    if (payload && payload_len) {
        out.insert(out.end(), payload, payload + payload_len);
    }
    return out;
}

} /* namespace cpp_streamer */
