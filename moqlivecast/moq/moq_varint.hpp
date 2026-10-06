#ifndef MEDIA_OVER_WT_MOQ_VARINT_HPP
#define MEDIA_OVER_WT_MOQ_VARINT_HPP

#include <cstddef>
#include <cstdint>
#include <vector>

/* MOQT 的整数编码 —— vi64。
 *
 * 【2026-09-25 起线上用的就是这套】。在此之前用的是 QUIC 变长整数（RFC 9000 §16），
 * 换成 vi64 的原因和依据见 docs/moq-transport-gap.md 的 G1：
 * draft-ietf-moq-transport-17 起 §1.4.1 明确规定 MOQT 的变长整数必须具备
 * 「1 字节尽可能能装」和「能表示完整 uint64」两条性质，而 QUIC 变长整数两条都不满足
 * （首字节只有 6 位可用，1 字节仅 0~63；上限 2^62-1）。两套编码【只有 0~63 一致】。
 *
 * 注意分层：quic/quic_packet.h 的 quic_varint_* 是 QUIC 层自用的（帧类型、传输参数、
 * stream ID…），那套【没变也不该变】，与本文件无关。
 *
 * 编码布局（长度 = 首字节前导 1 的个数 + 1）：
 *
 *   0xxxxxxx                          1B    7 位   0 ~ 127
 *   10xxxxxx xxxxxxxx                 2B   14 位
 *   110xxxxx xxxxxxxx xxxxxxxx        3B   21 位
 *   1110xxxx ...                      4B   28 位
 *   11110xxx ...                      5B   35 位
 *   111110xx ...                      6B   42 位
 *   1111110x ...                      7B   49 位
 *   11111110 x7                       8B   56 位
 *   11111111 x8                       9B   64 位
 *
 * 【非最小编码允许收】：draft-20 §1.4.1 原文 "Variable-length integers do not need
 * to be encoded using the minimum number of bytes; any encoding length that can
 * represent the value is valid. ... the value 0 can be encoded as 0x00, 0x8000,
 * 0xc00000, or any longer form." 该条来自 #1595，落在 draft-18（draft-17 曾禁 7 字节
 * 形态，同批解禁）。所以 non_minimal 出参【只作提示，不构成断连理由】，
 * 保留它是为了 Secure Objects 那类要求 AAD 逐字节一致的场景。
 * 我们发端只发最短形态是【自己的选择】（省字节、同一值只有一种字节形态），
 * 不是草案要求 —— 草案对收发两侧都不要求最小化。 */
namespace cpp_streamer {

/* 读一个 vi64。成功返回 true 并推进 off；数据不足返回 false（不推进 off）。
 * non_minimal（可选出参）：本次编码不是最短形态 → 置 true。见文件头说明。 */
inline bool MoqReadVi64(const uint8_t *p, size_t len, size_t &off, uint64_t &out,
                        bool *non_minimal = nullptr) {
    if (non_minimal) *non_minimal = false;
    if (!p || off >= len) return false;
    const uint8_t first = p[off];
    size_t lead = 0;
    while (lead < 8 && (first & (0x80u >> lead)) != 0) lead++;
    const size_t n = lead + 1;                    /* 总长度 1~9 字节 */
    if (off + n > len) return false;
    uint64_t v = (n <= 8) ? (first & ((1u << (8 - n)) - 1u)) : 0;
    for (size_t i = 1; i < n; i++) {
        v = (v << 8) | p[off + i];
    }
    if (n >= 2 && v < (1ull << (7 * (n - 1)))) {
        if (non_minimal) *non_minimal = true;
    }
    off += n;
    out = v;
    return true;
}

/* 写一个 vi64，总是最短形态。 */
inline void MoqAppendVi64(std::vector<uint8_t> &o, uint64_t v) {
    size_t n = 1;
    while (n <= 8 && v >= (1ull << (7 * n))) n++;
    if (n <= 8) {
        const size_t lead = n - 1;
        const uint8_t mask = (lead == 0)
            ? static_cast<uint8_t>(0x00)
            : static_cast<uint8_t>(0xFFu << (8 - lead));
        o.push_back(static_cast<uint8_t>(
            mask | ((v >> (8 * (n - 1))) & ((1u << (8 - n)) - 1u))));
    } else {
        o.push_back(static_cast<uint8_t>(0xFF));
    }
    for (size_t i = 1; i < n; i++) {
        o.push_back(static_cast<uint8_t>((v >> (8 * (n - 1 - i))) & 0xffu));
    }
}

/* ── 定长字段 ────────────────────────────────────────────────────────────
 * 下面两个【不是 varint，也不属于 MOQT 标准帧格式】—— 是本实现自定的
 * 「vi64 type + u16 len + body」控制消息头里的那个 u16。
 * 草案是 vi64 type + vi64 length，改成那样属于消息帧格式的差距（G6/G10），
 * 不在 G1 范围内，故此处保持原样。 */
inline bool MoqReadU16(const uint8_t *p, size_t len, size_t &off, uint16_t &out) {
    if (!p || off + 2 > len) return false;
    out = static_cast<uint16_t>((p[off] << 8) | p[off + 1]);
    off += 2;
    return true;
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
