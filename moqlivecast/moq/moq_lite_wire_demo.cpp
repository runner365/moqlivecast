/* moq-lite 编码侧（发布端角色）的字节级自检。
 *
 * 为什么必须有它：moq_lite_wire.hpp 里那几个 MlBuild… / MlAppend… 全部是
 * 「我们是发布端、对端是订阅端」时才走的路，而**字段顺序或长度错一个字节，
 * 线上不会报错**——对端只会静默地解出另一个数，或者干脆一直等一条永不到来的
 * 消息。这类错误编译期看不出来、跑起来也只在拉流端表现为花屏/静音。
 * 所以先在纯函数层面把字节钉死，再往 QUIC 里接。
 *
 * 编译（独立可执行，不编进 moqlivecast 主 target —— CMake 按 `_demo` 后缀
 * 排除，与 moq_varint_demo.cpp 同一套做法）：
 *
 *   c++ -std=c++14 -I moqlivecast/moq -I quic -I logger \
 *       moqlivecast/moq/moq_lite_wire_demo.cpp \
 *       quic/quic_packet.c logger/logger.c \
 *       -Wl,-undefined,dynamic_lookup -lpthread \
 *       -o /tmp/moq_lite_wire_demo && /tmp/moq_lite_wire_demo
 *
 * ⚠️ 那个 `-Wl,-undefined,dynamic_lookup`（macOS）是**为了绕开链接期的 openssl**：
 * 本文件唯一需要的是 quic_packet.c 里的 quic_varint_encode/decode/len，而同一个
 * 目标文件里还有一堆 quic_crypto_* 引用（要 openssl）。那六个符号自检从来不会
 * 调到，让它们留成动态查找即可。**varint 用的是真实现**，不是抄一份 —— 抄一份
 * 就变成「用自己的实现对答案」，这个自检就没意义了。
 *
 * 覆盖四件事：
 *   1. QUIC varint 的已知向量（含参考实现自带的跨语言断言里的那两个）
 *   2. 每条消息的**逐字节**断言，值来自 docs 里手算 + 参考实现源码核对
 *   3. 结构自洽：编出来的字节用**解码侧**的 MlReadMessage 切回来，
 *      body 边界必须不差分毫（多一个少一个字节都能在这里现形）
 *   4. zigzag 的边界（FRAME 的时间戳差靠它） */

#include "moq_lite_wire.hpp"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

using namespace cpp_streamer;

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

/* 逐字节比对，失败时把两边都打出来 —— 只打一句 "mismatch" 没法定位是哪个字段。 */
static void BytesEq(const std::vector<uint8_t> &got, const std::vector<uint8_t> &want,
                    const std::string &what) {
    if (got == want) {
        g_pass++;
        return;
    }
    g_fail++;
    std::printf("  FAIL  %s\n        got  = %s\n        want = %s\n", what.c_str(),
                Hex(got).c_str(), Hex(want).c_str());
}

static std::vector<uint8_t> Bytes(std::initializer_list<uint8_t> il) {
    return std::vector<uint8_t>(il);
}

/* ============================================================
 * 1. varint 已知向量
 * ============================================================ */

static void VarintVectors() {
    std::printf("[1] QUIC varint (RFC 9000 §16)\n");

    /* 长度分档的四个边界：1/2/4/8 字节形式各自的最小与最大。
     * 分档表在 quic/quic_packet.c:67-72（<=63 / <=16383 / <=2^30-1 / else）。 */
    struct { uint64_t v; size_t len; } kCases[] = {
        {0, 1}, {63, 1},
        {64, 2}, {16383, 2},
        {16384, 4}, {1000000, 4}, {1073741823ull, 4},
        {1073741824ull, 8}, {(uint64_t{1} << 62) - 1, 8},
    };
    for (size_t i = 0; i < sizeof(kCases) / sizeof(kCases[0]); i++) {
        std::vector<uint8_t> b;
        const bool wrote = MlWriteVarint(b, kCases[i].v);
        char label[96];
        std::snprintf(label, sizeof(label), "varint %llu -> %zu byte(s)",
                      (unsigned long long)kCases[i].v, kCases[i].len);
        ok(wrote && b.size() == kCases[i].len, label);
        if (!wrote) continue;

        size_t off = 0;
        uint64_t back = 0;
        const bool read = MlReadVarint(b.data(), b.size(), off, back);
        char rt[96];
        std::snprintf(rt, sizeof(rt), "varint %llu round-trips (got %llu)",
                      (unsigned long long)kCases[i].v, (unsigned long long)back);
        ok(read && back == kCases[i].v && off == b.size(), rt);
    }

    /* 参考实现自带的跨语言字节断言 [06 00 00 53 88 43 E8]（Lite05 的
     * TrackInfo 默认值）里那两个 varint —— 手算 max_age=5000、timescale=1000
     * 得到的，正是它对上的前提。这里对一遍，等于独立验证了「我怎么读那条断言」。 */
    { std::vector<uint8_t> b; MlWriteVarint(b, 5000);
      BytesEq(b, Bytes({0x53, 0x88}), "varint 5000 = 53 88 (from the ref vector)"); }
    { std::vector<uint8_t> b; MlWriteVarint(b, 1000);
      BytesEq(b, Bytes({0x43, 0xE8}), "varint 1000 = 43 E8 (from the ref vector)"); }

    /* 1000000 只能走 4 字节形式。这条是**真踩过的坑**：手算时写过
     * `8F 42 40` 这么一个三字节量，而 QUIC varint 根本没有 3 字节形式。 */
    { std::vector<uint8_t> b; MlWriteVarint(b, 1000000);
      BytesEq(b, Bytes({0x80, 0x0F, 0x42, 0x40}),
              "varint 1000000 = 80 0F 42 40 (4-byte form, NOT 3)"); }

    /* 超上限（>= 2^62）必须拒绝：底层的 quic_varint_encode 会静默截断。 */
    { std::vector<uint8_t> b;
      ok(!MlWriteVarint(b, uint64_t{1} << 62), "varint 2^62 is rejected");
      ok(b.empty(), "rejected varint writes nothing"); }
}

/* ============================================================
 * 2. 逐字节断言
 * ============================================================ */

static void TrackInfo() {
    std::printf("[2] TRACK_INFO (Track stream 0x6)\n");

    /* Legacy 内容：timescale 必须是 1000000（内层微秒前缀同一个尺度），
     * max_age 5000ms、priority 0 是本端固定值。
     *   size=7 | priority=0 (u8) | max_age=5000 | timescale=1000000
     *           00                 53 88           80 0F 42 40 */
    std::vector<uint8_t> out;
    ok(MlBuildTrackInfo(out, 0, 5000, 1000000), "build TRACK_INFO");
    BytesEq(out, Bytes({0x07, 0x00, 0x53, 0x88, 0x80, 0x0F, 0x42, 0x40}),
            "TRACK_INFO legacy = 07 00 53 88 80 0F 42 40");

    /* 结构自洽：用解码侧的 MlReadMessage 切一刀，body 必须**恰好**是
     * priority + max_age + timescale 三项，既不能剩字节也不能不够。
     * 这条专门盯「照 Lite05 的样例抄、多带一个 group-order 字节」那种错。 */
    size_t off = 0, blen = 0;
    const MlMsgStatus s = MlReadMessage(out.data(), out.size(), off, blen);
    ok(s == kMlMsgOk, "TRACK_INFO size prefix is readable");
    ok(off + blen == out.size(), "TRACK_INFO size covers the body exactly");

    size_t p = off;
    const uint8_t prio = out[p++];
    uint64_t max_age = 0, timescale = 0;
    const bool v1 = MlReadVarint(out.data(), off + blen, p, max_age);
    const bool v2 = MlReadVarint(out.data(), off + blen, p, timescale);
    ok(prio == 0, "TRACK_INFO priority = 0");
    ok(v1 && max_age == 5000, "TRACK_INFO max_age = 5000");
    ok(v2 && timescale == 1000000, "TRACK_INFO timescale = 1000000");
    ok(p == off + blen, "TRACK_INFO body has no trailing bytes");

    /* 非零 priority 也是 1 字节（u8 而非 varint）—— 这是那个「多一个字节」的
     * 错误最可能藏身的地方：priority=7 在 u8 下还是 `07`，在 varint 下也是
     * `07`，但 priority=200 在 u8 下是 `C8`、在 varint 下是 `40 C8`。
     * ⚠️ 长度字段只数 body：priority(1) + max_age=0(1) + timescale=1000(2)
     * = 4，不是 5。（max_age/timescale 的字节数不含在长度里，别把它自己算进去。） */
    std::vector<uint8_t> big;
    ok(MlBuildTrackInfo(big, 200, 0, 1000), "build TRACK_INFO priority=200");
    BytesEq(big, Bytes({0x04, 0xC8, 0x00, 0x43, 0xE8}),
            "TRACK_INFO priority is u8, not varint (200 -> C8)");
}

static void Announce() {
    std::printf("[3] ANNOUNCE stream (bidi 0x1)\n");

    /* ANNOUNCE_OK{origin=0, active=1} -> [size=2][00][01] */
    { std::vector<uint8_t> out;
      ok(MlBuildAnnounceOk(out, 0, 1), "build ANNOUNCE_OK");
      BytesEq(out, Bytes({0x02, 0x00, 0x01}), "ANNOUNCE_OK origin=0 active=1"); }

    /* active=0（本端没有可播的流，但**必须保持流打开**继续等）*/
    { std::vector<uint8_t> out;
      ok(MlBuildAnnounceOk(out, 0, 0), "build ANNOUNCE_OK active=0");
      BytesEq(out, Bytes({0x02, 0x00, 0x00}), "ANNOUNCE_OK origin=0 active=0"); }

    /* ANNOUNCE_START 最常见的一形：广播的路由恰好等于订阅端请求的 prefix，
     * 于是 suffix 是空串，hops 空链，cost {0,0}。
     *   body = 00(suffix "") 00(hops count) 00(warm) 00(cold) -> size=4 */
    { std::vector<uint8_t> out;
      ok(MlAppendAnnounceStart(out, "", 0, 0), "build ANNOUNCE_START");
      BytesEq(out, Bytes({0x00, 0x04, 0x00, 0x00, 0x00, 0x00}),
              "ANNOUNCE_START suffix=\"\" hops=[] cost={0,0}"); }

    /* 带 suffix 的一形（前缀不完全匹配时才有），顺带验证 (s) 的长度前缀。 */
    { std::vector<uint8_t> out;
      ok(MlAppendAnnounceStart(out, "hang", 0, 0), "build ANNOUNCE_START suffix=hang");
      BytesEq(out, Bytes({0x00, 0x08, 0x04, 'h', 'a', 'n', 'g',
                          0x00, 0x00, 0x00}),
              "ANNOUNCE_START suffix=\"hang\""); }

    /* ANNOUNCE_END{id=42}：本仓注释里记的实测样例就是 01 01 2A。 */
    { std::vector<uint8_t> out;
      ok(MlAppendAnnounceEnd(out, 42), "build ANNOUNCE_END");
      BytesEq(out, Bytes({0x01, 0x01, 0x2A}), "ANNOUNCE_END id=42 = 01 01 2A"); }
}

static void Subscribe() {
    std::printf("[4] SUBSCRIBE stream (bidi 0x2)\n");

    /* Lite06 上**没有 SUBSCRIBE_OK**：接受是隐式的，第一条应答就是
     * SUBSCRIBE_START。这条断言防的是「照着旧草案发一条 SUBSCRIBE_OK」——
     * 真发了的话对端会把它的 body 当成一条 START 解。 */
    { std::vector<uint8_t> out;
      ok(MlBuildSubscribeStart(out, 0), "build SUBSCRIBE_START");
      BytesEq(out, Bytes({0x00, 0x01, 0x00}), "SUBSCRIBE_START group=0"); }

    /* ⚠️ group=1000 是 **2** 字节 varint（43 E8），所以长度字段是 02。
     * 手写向量时最容易把它写成 03（把长度字段自己也数进去）。 */
    { std::vector<uint8_t> out;
      ok(MlBuildSubscribeStart(out, 1000), "build SUBSCRIBE_START group=1000");
      BytesEq(out, Bytes({0x00, 0x02, 0x43, 0xE8}), "SUBSCRIBE_START group=1000"); }

    { std::vector<uint8_t> out;
      ok(MlBuildSubscribeEnd(out, 1000), "build SUBSCRIBE_END");
      BytesEq(out, Bytes({0x01, 0x02, 0x43, 0xE8}), "SUBSCRIBE_END group=1000"); }
}

static void GroupAndFrame() {
    std::printf("[5] GROUP / FRAME (uni 0x0)\n");

    /* GROUP 头。⚠️ 它不是流的全部：流的第一字节是 STREAM_TYPE=0x00，
     * 由调用方先写。这里只断言消息本体。 */
    { std::vector<uint8_t> out;
      ok(MlBuildGroupHeader(out, 1, 0, 0), "build GROUP header");
      BytesEq(out, Bytes({0x03, 0x01, 0x00, 0x00}),
              "GROUP sub=1 seq=0 frame_start=0"); }

    /* frame_start 是 06 新增的第三个 varint —— 少了它不会报错，只会让组内
     * 第一个 FRAME 的时间戳差整体错位。用非零值断言它确实在。 */
    { std::vector<uint8_t> out;
      ok(MlBuildGroupHeader(out, 1, 1000, 7), "build GROUP header seq=1000 fs=7");
      BytesEq(out, Bytes({0x04, 0x01, 0x43, 0xE8, 0x07}),
              "GROUP sub=1 seq=1000 frame_start=7"); }

    /* FRAME：差 + 长度 + 载荷，背靠背，没有类型字节。 */
    { std::vector<uint8_t> out;
      ok(MlAppendFrame(out, 0, nullptr, 0), "build empty FRAME");
      BytesEq(out, Bytes({0x00, 0x00}), "FRAME delta=0 empty payload = 00 00"); }

    /* 负数差：zigzag(-21) = 41 = 0x29。音频帧间差就是这种量级。 */
    { std::vector<uint8_t> out;
      ok(MlAppendFrame(out, -21, nullptr, 0), "build FRAME delta=-21");
      BytesEq(out, Bytes({0x29, 0x00}), "FRAME delta=-21 -> zigzag 0x29"); }

    /* 真实量级：z 视频帧的绝对微秒数（10062000 = 本仓实测里那个
     * `80 99 88 b0` 内层前缀）zigzag 后是 20124000 = 0x81331160。 */
    { std::vector<uint8_t> out;
      const uint8_t payload[3] = {0xAA, 0xBB, 0xCC};
      ok(MlAppendFrame(out, 10062000, payload, sizeof(payload)), "build FRAME delta=10062000");
      BytesEq(out, Bytes({0x81, 0x33, 0x11, 0x60, 0x03, 0xAA, 0xBB, 0xCC}),
              "FRAME delta=10062000 payload=3"); }

    /* 结构自洽：一组「组头 + 两帧」连起来，用解码侧的读法逐条切，
     * 必须正好切完、不留尾巴。这条是整条下行链路的形状演练。 */
    {
        std::vector<uint8_t> out;
        MlAppendVarint(out, 0x00);                    /* STREAM_TYPE = Group */
        MlBuildGroupHeader(out, 1, 0, 0);
        const uint8_t f1[4] = {0x01, 0x02, 0x03, 0x04};
        MlAppendFrame(out, 0, f1, sizeof(f1));
        const uint8_t f2[2] = {0x05, 0x06};
        MlAppendFrame(out, 40, f2, sizeof(f2));

        /* ⚠️ 第二帧的差是 40 → zigzag 80 → **2** 字节（>63 换档），
         * 所以那一帧是 2+1+2 = 5 而不是 4。手算时按「差总是 1 字节」就会
         * 少算这一格 —— 这正是这条断言要盯的。 */
        ok(out.size() == 1 + 4 + 6 + 5, "group stream total length");

        size_t pos = 0;
        uint64_t st = 0;
        ok(MlReadVarint(out.data(), out.size(), pos, st) && st == 0x00,
           "group stream starts with STREAM_TYPE 0x00");

        /* 组头：读完之后第一帧必须紧跟其后，一格不差 */
        size_t hoff = pos, hlen = 0;
        ok(MlReadMessage(out.data(), out.size(), hoff, hlen) == kMlMsgOk,
           "group header is readable");
        ok(hoff + hlen == 1 + 4, "group header ends where the first frame begins");
        pos = hoff + hlen;

        /* 两帧：各自的差与载荷都要能原样取回 */
        const int64_t kDeltas[2] = {0, 40};
        const size_t kLens[2] = {4, 2};
        for (int i = 0; i < 2; i++) {
            uint64_t zz = 0;
            ok(MlReadVarint(out.data(), out.size(), pos, zz), "frame delta readable");
            ok(MlZigzagDecode(zz) == kDeltas[i], "frame delta value");
            size_t foff = pos, flen = 0;
            ok(MlReadMessage(out.data(), out.size(), foff, flen) == kMlMsgOk,
               "frame payload is readable");
            ok(flen == kLens[i], "frame payload length");
            pos = foff + flen;
        }
        ok(pos == out.size(), "group stream is consumed exactly (no trailing bytes)");
    }
}

/* ============================================================
 * 3. 两条真实下行流的完整形状（发布端角色）
 *
 * 上面各段是**单条消息**的字节；这里把「一条 uni 流从头到尾长什么样」拼
 * 出来，并按 M3 收侧（MoqLiteSession::ParseGroupStream / OnMediaFrame）的
 * 读法逐条切回去。这两段形状是发布端新写的，没有别的调用者会替它们报错 ——
 * 写错了只会让对端静默错位。
 * ============================================================ */

/* 媒体轨：一组两帧 + 视频结束哨兵。
 *
 * payload 的**第二层**是 Legacy 容器的约定：`[绝对微秒 varint][裸编解码字节]`。
 * 这层最容易漏 —— 漏了的话对端拿到的是一个以微秒整数开头的"NALU"，现象是
 * 花屏而不是报错（M3 收侧实测踩过：视频帧首 4 字节 80 99 88 b0 = 10062000）。 */
static void LegacyGroupStream() {
    std::printf("[7] legacy media group stream (one GOP, video sentinel)\n");

    const int64_t kTs1Us = 10062000;   /* 组内第 1 帧：差对它取 0，所以差 = 它自己 */
    const int64_t kTs2Us = 10066000;   /* 第 2 帧：差 = +4000us（25fps） */

    std::vector<uint8_t> stream;
    MlAppendVarint(stream, 0x00);                 /* STREAM_TYPE = Group */
    ok(MlBuildGroupHeader(stream, /*sub_id=*/1, /*seq=*/0, /*frame_start=*/0),
       "media: GROUP header");

    /* 帧 1：payload = [微秒 varint][codec bytes] */
    std::vector<uint8_t> p1;
    ok(MlWriteVarint(p1, (uint64_t)kTs1Us), "media: frame1 inner us prefix");
    p1.insert(p1.end(), {0x00, 0x00, 0x02, 0x72});   /* 4 字节长度前缀的 NALU */
    ok(MlAppendFrame(stream, kTs1Us - 0, p1.data(), p1.size()), "media: frame1");

    /* 帧 2 */
    std::vector<uint8_t> p2;
    MlWriteVarint(p2, (uint64_t)kTs2Us);
    p2.insert(p2.end(), {0x00, 0x00, 0x01, 0x02});
    ok(MlAppendFrame(stream, kTs2Us - kTs1Us, p2.data(), p2.size()), "media: frame2");

    /* 结束哨兵：**payload 不是空的** —— 是「微秒 varint + 零字节载荷」，
     * 差值相对上一帧取 0（它记的就是组的结束时刻 = 最后一帧的时间戳）。 */
    std::vector<uint8_t> p3;
    MlWriteVarint(p3, (uint64_t)kTs2Us);
    ok(MlAppendFrame(stream, 0, p3.data(), p3.size()), "media: video sentinel");

    /* ── 按收侧的读法切回去 ── */
    size_t pos = 0;
    uint64_t st = 0;
    ok(MlReadVarint(stream.data(), stream.size(), pos, st) && st == 0x00,
       "media: STREAM_TYPE is Group (0x00)");

    size_t hoff = pos, hlen = 0;
    ok(MlReadMessage(stream.data(), stream.size(), hoff, hlen) == kMlMsgOk,
       "media: GROUP header readable");
    {   /* 组头本体：sub_id / seq / frame_start 三个 varint，顺序不能换 */
        size_t bp = hoff;
        uint64_t sub = 0, seq = 0, fs = 0;
        ok(MlReadVarint(stream.data(), hoff + hlen, bp, sub) && sub == 1,
           "media: GROUP sub_id");
        ok(MlReadVarint(stream.data(), hoff + hlen, bp, seq) && seq == 0,
           "media: GROUP seq");
        ok(MlReadVarint(stream.data(), hoff + hlen, bp, fs) && fs == 0,
           "media: GROUP frame_start present (the 06 field)");
        ok(bp == hoff + hlen, "media: GROUP header consumed exactly");
    }
    pos = hoff + hlen;

    /* 三帧：差解码回来必须是【绝对、绝对、0】—— 「每组第一帧的差就是它的
     * 绝对时间戳」这条不变量就是靠它成立的。 */
    const int64_t kWantDelta[3] = {kTs1Us, kTs2Us - kTs1Us, 0};
    const uint64_t kWantInner[3] = {(uint64_t)kTs1Us, (uint64_t)kTs2Us, (uint64_t)kTs2Us};
    const size_t kWantCodec[3] = {4, 4, 0};   /* 哨兵的编解码载荷是 0 字节 */
    for (int i = 0; i < 3; i++) {
        uint64_t zz = 0;
        ok(MlReadVarint(stream.data(), stream.size(), pos, zz), "media: frame delta readable");
        ok(MlZigzagDecode(zz) == kWantDelta[i], "media: frame delta value");
        size_t foff = pos, flen = 0;
        ok(MlReadMessage(stream.data(), stream.size(), foff, flen) == kMlMsgOk,
           "media: frame payload readable");
        /* payload 里第一件事就是内层微秒 */
        size_t ip = foff;
        uint64_t inner = 0;
        ok(MlReadVarint(stream.data(), foff + flen, ip, inner), "media: inner us readable");
        ok(inner == kWantInner[i], "media: inner us equals the accumulated outer ts");
        ok(foff + flen - ip == kWantCodec[i], "media: codec payload length");
        pos = foff + flen;
    }
    ok(pos == stream.size(), "media: group stream consumed exactly (no trailing bytes)");
}

/* catalog 轨：一条 uni 流只有一组、一组只有一帧，**帧载荷就是裸 JSON**，
 * 没有 Legacy 那层微秒前缀（hang/src/catalog/consumer.rs:95-99）。 */
static void CatalogGroupStream() {
    std::printf("[8] catalog group stream (raw JSON, no inner us prefix)\n");

    const std::string json = "{\"video\":{\"renditions\":{\"v0\":{}}}}";

    std::vector<uint8_t> stream;
    MlAppendVarint(stream, 0x00);                  /* STREAM_TYPE = Group */
    ok(MlBuildGroupHeader(stream, 1, /*seq=*/0, 0), "catalog: GROUP header");
    ok(MlAppendFrame(stream, /*delta=*/0,
                     reinterpret_cast<const uint8_t *>(json.data()), json.size()),
       "catalog: single frame");

    size_t pos = 0;
    uint64_t st = 0;
    ok(MlReadVarint(stream.data(), stream.size(), pos, st) && st == 0x00,
       "catalog: STREAM_TYPE is Group");

    size_t hoff = pos, hlen = 0;
    ok(MlReadMessage(stream.data(), stream.size(), hoff, hlen) == kMlMsgOk,
       "catalog: GROUP header readable");
    pos = hoff + hlen;

    uint64_t zz = 0;
    ok(MlReadVarint(stream.data(), stream.size(), pos, zz) && MlZigzagDecode(zz) == 0,
       "catalog: outer frame delta is 0");

    size_t foff = pos, flen = 0;
    ok(MlReadMessage(stream.data(), stream.size(), foff, flen) == kMlMsgOk,
       "catalog: frame readable");
    /* ⚠️ 这里**不能**再剥一层 varint：payload 就是 JSON 本身。多剥一层是
     * 最隐蔽的一种错 —— 对端会把 JSON 的第一个字符当长度前缀，然后解出个
     * 空串，表现是「announce 有这条广播但 catalog 是空的」。 */
    ok(flen == json.size(), "catalog: payload length is the whole JSON");
    ok(std::string(reinterpret_cast<const char *>(stream.data() + foff), flen) == json,
       "catalog: payload is byte-identical to the stored JSON");
    ok(foff + flen == stream.size(), "catalog: stream consumed exactly");
}

/* ============================================================
 * 4. zigzag 的边界
 * ============================================================ */

static void Zigzag() {
    std::printf("[6] zigzag (FRAME timestamp delta)\n");

    struct { int64_t in; uint64_t out; } kCases[] = {
        {0, 0}, {-1, 1}, {1, 2}, {-2, 3}, {2, 4}, {-21, 41},
        {kMlZigzagMax, (uint64_t{1} << 62) - 2},
        {kMlZigzagMin, (uint64_t{1} << 62) - 1},
    };
    for (size_t i = 0; i < sizeof(kCases) / sizeof(kCases[0]); i++) {
        uint64_t zz = 0;
        const bool enc = MlZigzagEncode(kCases[i].in, zz);
        char label[96];
        std::snprintf(label, sizeof(label), "zigzag(%lld)", (long long)kCases[i].in);
        ok(enc && zz == kCases[i].out, label);
        if (enc) {
            ok(MlZigzagDecode(zz) == kCases[i].in, std::string(label) + " decodes back");
        }
    }

    /* 越界必须拒绝 —— 62 位的 varint 装不下。 */
    { uint64_t zz = 0;
      ok(!MlZigzagEncode(kMlZigzagMax + 1, zz), "zigzag rejects 2^61");
      ok(!MlZigzagEncode(kMlZigzagMin - 1, zz), "zigzag rejects -2^61-1"); }
}

int main() {
    std::printf("moq-lite wire encoder self-test\n\n");
    VarintVectors();
    TrackInfo();
    Announce();
    Subscribe();
    GroupAndFrame();
    LegacyGroupStream();
    CatalogGroupStream();
    Zigzag();
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
