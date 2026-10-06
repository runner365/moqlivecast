/* vi64 编码的自检程序 —— MOQT 现在线上用的整数编码。
 *
 * 纯头文件依赖，独立编译，不进 moqlivecast 主 target（CMake 里按 `_demo` 后缀
 * 排除，也没有自己的 target）：
 *
 *   c++ -std=c++14 moqlivecast/moq/moq_varint_demo.cpp -o /tmp/moq_varint_demo \
 *       && /tmp/moq_varint_demo
 *
 * 覆盖四件事：
 *   1. 长度分档表（1~9 字节各自的边界值）
 *   2. 往返一致性
 *   3. 已知字节序列
 *   4. 非最小编码的检出（只是提示：草案【允许】收非最小编码，不该因此断连）
 *   5. 草案 draft-20 §1.4.1 Table 2 的权威向量逐条对
 *
 * 事实依据（已核对原文，非二手转述）：draft-ietf-moq-transport-20 §1.4.1
 * 明确 "Variable-length integers do not need to be encoded using the minimum
 * number of bytes"，且 Table 1 含 1111110 / 7 字节形态。
 * 该条来自 #1595，落在 Appendix A.3 "Since draft-17"（即 draft-18 起）。
 *
 * 换编码的依据见 docs/moq-transport-gap.md 的 G1。注意旧的 QUIC 变长整数
 * 实现已从 moq_varint.hpp 删除，所以这里不再有两者对比的用例。 */

#include "moq_varint.hpp"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

using cpp_streamer::MoqAppendVi64;
using cpp_streamer::MoqReadVi64;

static int g_pass = 0;
static int g_fail = 0;

static void ok(bool cond, const std::string &what) {
    if (cond) {
        g_pass++;
    } else {
        g_fail++;
        std::printf("  FAIL  %s\n", what.c_str());
    }
}

static std::string Hex(const std::vector<uint8_t> &v) {
    std::string s;
    char b[4];
    for (size_t i = 0; i < v.size(); i++) {
        std::snprintf(b, sizeof(b), "%02X", v[i]);
        if (i) s += ' ';
        s += b;
    }
    return s;
}

/* 把值编出来再解回去，要求字节数、值都一致。 */
static void RoundTrip(uint64_t v, size_t want_len, const char *label) {
    std::vector<uint8_t> buf;
    MoqAppendVi64(buf, v);

    char msg[160];
    std::snprintf(msg, sizeof(msg), "%s: len=%zu 期望 %zu  (%s)",
                  label, buf.size(), want_len, Hex(buf).c_str());
    ok(buf.size() == want_len, msg);

    size_t off = 0;
    uint64_t back = 0;
    bool non_min = true;
    const bool read_ok = MoqReadVi64(buf.data(), buf.size(), off, back, &non_min);

    std::snprintf(msg, sizeof(msg), "%s: 解回失败", label);
    ok(read_ok, msg);
    std::snprintf(msg, sizeof(msg), "%s: 值不一致 (%llu)",
                  label, (unsigned long long)back);
    ok(back == v, msg);
    std::snprintf(msg, sizeof(msg), "%s: off 没走到末尾", label);
    ok(off == buf.size(), msg);
    std::snprintf(msg, sizeof(msg), "%s: 自己编出来的被判成非最小", label);
    ok(!non_min, msg);
}

static void ExpectBytes(uint64_t v, const std::vector<uint8_t> &want,
                        const char *label) {
    std::vector<uint8_t> buf;
    MoqAppendVi64(buf, v);
    ok(buf == want, std::string(label) + ": 字节序列不符，实得 " + Hex(buf));
}

int main() {
    /* ── 1 + 2. 长度分档与往返 ────────────────────────────────────────
     * 每档的边界：n 字节能表示 [2^(7(n-1)), 2^(7n) - 1]（n<=8），
     * 9 字节表示 [2^56, 2^64 - 1]。 */
    std::printf("[1] 长度分档 + 往返\n");
    RoundTrip(0ull, 1, "0");
    RoundTrip(1ull, 1, "1");
    RoundTrip(127ull, 1, "127 (1B 上界)");
    RoundTrip(128ull, 2, "128 (2B 下界)");
    RoundTrip(16383ull, 2, "16383 (2B 上界)");
    RoundTrip(16384ull, 3, "16384 (3B 下界)");
    RoundTrip((1ull << 21) - 1, 3, "2^21-1 (3B 上界)");
    RoundTrip(1ull << 21, 4, "2^21 (4B 下界)");
    RoundTrip((1ull << 28) - 1, 4, "2^28-1 (4B 上界)");
    RoundTrip(1ull << 28, 5, "2^28 (5B 下界)");
    RoundTrip((1ull << 35) - 1, 5, "2^35-1 (5B 上界)");
    RoundTrip(1ull << 35, 6, "2^35 (6B 下界)");
    RoundTrip((1ull << 42) - 1, 6, "2^42-1 (6B 上界)");
    RoundTrip(1ull << 42, 7, "2^42 (7B 下界)");
    RoundTrip((1ull << 49) - 1, 7, "2^49-1 (7B 上界)");
    RoundTrip(1ull << 49, 8, "2^49 (8B 下界)");
    RoundTrip((1ull << 56) - 1, 8, "2^56-1 (8B 上界)");
    RoundTrip(1ull << 56, 9, "2^56 (9B 下界)");
    RoundTrip((1ull << 63), 9, "2^63");
    RoundTrip(~0ull, 9, "2^64-1 (9B 上界)");

    /* ── 3. 已知字节序列 ────────────────────────────────────────────── */
    std::printf("[2] 已知字节序列\n");
    ExpectBytes(0ull, {0x00}, "0");
    ExpectBytes(127ull, {0x7F}, "127");
    ExpectBytes(128ull, {0x80, 0x80}, "128");
    ExpectBytes(15293ull, {0xBB, 0xBD}, "15293");       /* 14 位 → 2 字节 */
    ExpectBytes(16384ull, {0xC0, 0x40, 0x00}, "16384"); /* 21 位 → 3 字节 */
    ExpectBytes(~0ull, {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF},
                "2^64-1");
    ExpectBytes(1ull << 56,
                {0xFF, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
                "2^56");

    /* ── 3b. 草案 draft-20 §1.4.1 Table 2 的权威向量 ──────────────────
     * 直接拿规范自己给的例子对，比自造边界值更有说服力。
     * 注意最后一行是 9 字节形态，以及第 2 行是【非最小】编码（0x8025 表示 37）。 */
    std::printf("[2b] 草案 Table 2 权威向量（draft-20 §1.4.1）\n");
    {
        struct { const char *hex; uint64_t val; } kTable[] = {
            {"25",                 37ull},
            {"8025",               37ull},                    /* 非最小，草案允许 */
            {"bbbd",               15293ull},
            {"ed7f3e7d",           226442877ull},
            {"faa1a0e403d8",       2893212287960ull},
            {"fc8998abc66bc0",     151288809941952ull},       /* 7 字节形态 */
            {"fefa318fa8e3ca11",   70423237261249041ull},
            {"ffffffffffffffffff", 18446744073709551615ull},  /* 9 字节形态 */
        };
        for (const auto &t : kTable) {
            std::vector<uint8_t> bytes;
            const std::string hex(t.hex);
            for (size_t i = 0; i + 1 < hex.size(); i += 2) {
                bytes.push_back(static_cast<uint8_t>(
                    std::stoul(hex.substr(i, 2), nullptr, 16)));
            }
            size_t off = 0;
            uint64_t v = 0;
            bool non_min = false;
            const bool r = MoqReadVi64(bytes.data(), bytes.size(), off, v, &non_min);
            char label[96];
            std::snprintf(label, sizeof(label), "草案 0x%s → %llu",
                          t.hex, (unsigned long long)t.val);
            ok(r && v == t.val, std::string(label) + " 解码不符");
            ok(off == bytes.size(), std::string(label) + " 长度不符");
            /* 反向：我们的编码器对同一个值必须产出最小编码，再解回来一致 */
            if (r) {
                std::vector<uint8_t> enc;
                MoqAppendVi64(enc, t.val);
                size_t o2 = 0;
                uint64_t v2 = 0;
                MoqReadVi64(enc.data(), enc.size(), o2, v2, nullptr);
                ok(v2 == t.val, std::string(label) + " 编码后回读不一致");
                ok(enc.size() <= bytes.size(),
                   std::string(label) + " 编码器产出的比草案样例还长");
            }
        }
    }

    /* ── 4. 非最小编码的检出 ────────────────────────────────────────
     * 注意语义：草案【允许】收非最小编码（IETF 125 编辑建议
     * "Allow 7 byte varints and non-minimal encodings"），所以这里测的是
     * "能识别出来"，【不是】"必须报错断连"。 */
    std::printf("[3] 非最小编码的检出（允许收，仅提示）\n");
    {
        /* 0x80 0x00 用 2 字节表示了 0，而 0 只需要 1 字节 → 非最小。 */
        const std::vector<uint8_t> bad = {0x80, 0x00};
        size_t off = 0;
        uint64_t v = 999;
        bool non_min = false;
        const bool r = MoqReadVi64(bad.data(), bad.size(), off, v, &non_min);
        ok(r && v == 0 && non_min, "0x80 0x00 应解出 0 且标记非最小");
        ok(off == 2, "0x80 0x00 应吃掉 2 字节");
    }
    {
        /* 0xC0 0x00 0x7F 用 3 字节表示了 127（1 字节就够）→ 非最小。 */
        const std::vector<uint8_t> bad = {0xC0, 0x00, 0x7F};
        size_t off = 0;
        uint64_t v = 0;
        bool non_min = false;
        const bool r = MoqReadVi64(bad.data(), bad.size(), off, v, &non_min);
        ok(r && v == 127 && non_min, "0xC0 0x00 0x7F 应解出 127 且标记非最小");
    }
    {
        /* 最小编码不该被误报。另外确认编码器只产出最小编码 ——
         * 收可以宽松，发必须规范。 */
        const std::vector<uint8_t> good = {0xBB, 0xBD};
        size_t off = 0;
        uint64_t v = 0;
        bool non_min = true;
        const bool r = MoqReadVi64(good.data(), good.size(), off, v, &non_min);
        ok(r && v == 15293 && !non_min, "0xBB 0xBD 是最小编码，不该被误报");

        std::vector<uint8_t> enc;
        MoqAppendVi64(enc, 15293);
        size_t off2 = 0;
        uint64_t v2 = 0;
        bool nm2 = false;
        MoqReadVi64(enc.data(), enc.size(), off2, v2, &nm2);
        /* 注意：草案【不】要求发端最小化（§1.4.1 明说非最小也合法），
         * 只发最短是我们自己的选择 —— 省字节，且保证同一值只有一种字节形态。
         * 判据是"我们的编码器不产非最小"，不是"非最小非法"。 */
        ok(!nm2, "我们的编码器只输出最小编码（自身约束，非草案要求）");
    }

    /* ── 5. 旧编码的字节在 vi64 下会被读成别的值 ──────────────────────
     * 这一节是「三端必须同时切换」的回归证据，不是因为线上还会收到旧编码。
     * 用旧 QUIC 变长整数的字节字面量喂给 vi64，证明它既不错位也不报错，
     * 只是【静默地】解出另一个值 —— 这正是漏改一端时的故障形态：
     * 不崩、不断连、没有日志，只是数字全错。 */
    std::printf("[4] 旧 QUIC 变长整数字节在 vi64 下的行为\n");
    {
        /* 100 在旧编码里是 0x40 0x64（2 字节）。vi64 下 0x40 前导 1 个数为 0
         * → 单字节 → 64；剩下的 0x64 又是一个独立值 100。
         * 也就是「一个值」被读成「两个值」，后续字段全体顺移。 */
        const std::vector<uint8_t> old_100 = {0x40, 0x64};
        size_t off = 0;
        uint64_t v1 = 0;
        const bool r1 = MoqReadVi64(old_100.data(), old_100.size(), off, v1);
        ok(r1 && v1 == 64 && off == 1, "旧编码的 0x40 0x64 在 vi64 下读成 64");
        uint64_t v2 = 0;
        const bool r2 = MoqReadVi64(old_100.data(), old_100.size(), off, v2);
        ok(r2 && v2 == 100 && off == 2, "剩余 0x64 被读成第二个值 100");
        std::printf("  提示：旧编码 100 = 40 64，vi64 读作两个值 64 / 100"
                    " —— 不报错，只是静默错位\n");
    }
    {
        /* 反过来也对：vi64 的 0x64 是 100，旧编码会把它当 2 字节的头。 */
        std::vector<uint8_t> vi;
        MoqAppendVi64(vi, 100);
        ok(vi.size() == 1 && vi[0] == 0x64, "vi64: 100 → 单字节 0x64");
        ok(!(vi.size() == 2 && vi[0] == 0x40), "vi64 的 100 不再是旧编码的 40 64");
    }
    {
        /* 0~63 是唯一一致的区间：64 以下两套编码字节相同，所以小值测不出问题，
         * 只有超过 63 才暴露 —— 这也是为什么「拿小消息联通」不能证明切换正确。 */
        bool low_ok = true;
        for (uint64_t v = 0; v <= 63; v++) {
            std::vector<uint8_t> enc;
            MoqAppendVi64(enc, v);
            if (enc.size() != 1 || enc[0] != static_cast<uint8_t>(v)) {
                low_ok = false;
                break;
            }
        }
        ok(low_ok, "0~63 与旧编码一致（都是单字节且等于自身）");
        std::vector<uint8_t> at64;
        MoqAppendVi64(at64, 64);
        ok(at64.size() == 1 && at64[0] == 0x40,
           "64 在 vi64 下仍是单字节 0x40 —— 与旧编码开始分道");
    }

    std::printf("\n结果：%d 通过，%d 失败\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
