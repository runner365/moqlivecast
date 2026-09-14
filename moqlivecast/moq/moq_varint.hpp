#ifndef MEDIA_OVER_WT_MOQ_VARINT_HPP
#define MEDIA_OVER_WT_MOQ_VARINT_HPP

#include <cstddef>
#include <cstdint>
#include <vector>

namespace cpp_streamer {

inline bool MoqReadVarint(const uint8_t *p, size_t len, size_t &off, uint64_t &out) {
    if (!p || off >= len) return false;
    const uint8_t first = p[off];
    const int n = 1 << (first >> 6);
    if (off + static_cast<size_t>(n) > len) return false;
    uint64_t v = first & 0x3f;
    for (int i = 1; i < n; i++) {
        v = (v << 8) | p[off + static_cast<size_t>(i)];
    }
    off += static_cast<size_t>(n);
    out = v;
    return true;
}

inline bool MoqReadU16(const uint8_t *p, size_t len, size_t &off, uint16_t &out) {
    if (!p || off + 2 > len) return false;
    out = static_cast<uint16_t>((p[off] << 8) | p[off + 1]);
    off += 2;
    return true;
}

inline void MoqAppendVarint(std::vector<uint8_t> &o, uint64_t v) {
    if (v < 64) {
        o.push_back(static_cast<uint8_t>(v));
        return;
    }
    if (v < 16384) {
        o.push_back(static_cast<uint8_t>(0x40 | (v >> 8)));
        o.push_back(static_cast<uint8_t>(v & 0xff));
        return;
    }
    if (v < (1ull << 30)) {
        o.push_back(static_cast<uint8_t>(0x80 | ((v >> 24) & 0x3f)));
        o.push_back(static_cast<uint8_t>((v >> 16) & 0xff));
        o.push_back(static_cast<uint8_t>((v >> 8) & 0xff));
        o.push_back(static_cast<uint8_t>(v & 0xff));
        return;
    }
    o.push_back(static_cast<uint8_t>(0xc0 | ((v >> 56) & 0x3f)));
    for (int i = 6; i >= 0; i--) {
        o.push_back(static_cast<uint8_t>((v >> (i * 8)) & 0xff));
    }
}

inline void MoqAppendU16(std::vector<uint8_t> &o, uint16_t v) {
    o.push_back(static_cast<uint8_t>(v >> 8));
    o.push_back(static_cast<uint8_t>(v & 0xff));
}

inline void MoqAppendBytes(std::vector<uint8_t> &o, const uint8_t *p, size_t n) {
    if (!p || n == 0) return;
    o.insert(o.end(), p, p + n);
}

} /* namespace cpp_streamer */

#endif /* MEDIA_OVER_WT_MOQ_VARINT_HPP */
