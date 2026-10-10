/* moq-lite 线编码的字节级自测 —— C 版。
 *
 * 与 moqlivecast/moq/moq_lite_wire_demo.cpp 用的是**同一批向量**（那边验 C++
 * 服务端，这里验本 API 的 C 编码器）。两边向量一致，才说明同一条线上两个
 * 实现写出的字节相同 —— 这是防错位的唯一防线：写错了不会编译报错，只会
 * 在对端静默错位（两套 varint 只有 0~63 一致）。
 *
 * 独立编译，不依赖 quic/ 或 libuv：
 *   cc -std=gnu11 -I. moqlite/moq_lite_wire.c moqlite/moq_lite_wire_test.c \
 *      -o /tmp/moqbuild/wire_test && /tmp/moqbuild/wire_test
 *
 * 退出码 0 = 全过。 */
#include "moq_lite_wire.h"

#include <stdio.h>
#include <string.h>

static int g_pass = 0;
static int g_fail = 0;

static void ok(int cond, const char *what) {
    if (cond) { g_pass++; return; }
    g_fail++;
    printf("  FAIL  %s\n", what);
}

/* 逐字节比对，失败时把两边都打出来 —— 静默错位类的问题只看「失败」没用，
 * 得看差在哪一个字节。 */
static void bytes_eq(const MoqBuf *b, const uint8_t *want, size_t n,
                     const char *what) {
    if (b->len == n && (n == 0 || memcmp(b->buf, want, n) == 0)) {
        g_pass++;
        return;
    }
    size_t i;
    g_fail++;
    printf("  FAIL  %s\n        got  (%zu):", what, b->len);
    for (i = 0; i < b->len; i++) printf(" %02X", b->buf[i]);
    printf("\n        want (%zu):", n);
    for (i = 0; i < n; i++) printf(" %02X", want[i]);
    printf("\n");
}

#define BYTES_EQ(b, ...) do {                                  \
        const uint8_t w_[] = {__VA_ARGS__};                    \
        bytes_eq((b), w_, sizeof(w_), #b);                     \
    } while (0)

/* ============================================================ */

static void test_varint(void) {
    printf("[1] varint (QUIC, RFC 9000 §16)\n");

    /* 参考实现自带的跨语言断言 [06 00 00 53 88 43 E8] 里那两个 varint。 */
    { MoqBuf b; moq_buf_init(&b);
      ok(moq_varint_write(&b, 5000) == 0, "write 5000");
      BYTES_EQ(&b, 0x53, 0x88);
      moq_buf_free(&b); }

    { MoqBuf b; moq_buf_init(&b);
      ok(moq_varint_write(&b, 1000) == 0, "write 1000");
      BYTES_EQ(&b, 0x43, 0xE8);
      moq_buf_free(&b); }

    /* 1_000_000 只能走 4 字节形式 —— QUIC varint 没有 3 字节档。 */
    { MoqBuf b; moq_buf_init(&b);
      ok(moq_varint_write(&b, 1000000) == 0, "write 1000000");
      BYTES_EQ(&b, 0x80, 0x0F, 0x42, 0x40);
      moq_buf_free(&b); }

    /* 各档的上下界，长度必须落对。 */
    ok(moq_varint_size(0) == 1,        "size(0) = 1");
    ok(moq_varint_size(63) == 1,       "size(63) = 1");
    ok(moq_varint_size(64) == 2,       "size(64) = 2 (boundary)");
    ok(moq_varint_size(16383) == 2,    "size(16383) = 2");
    ok(moq_varint_size(16384) == 4,    "size(16384) = 4 (boundary)");
    ok(moq_varint_size(1073741823) == 4, "size(2^30-1) = 4");
    ok(moq_varint_size(1073741824) == 8, "size(2^30) = 8 (boundary)");
    ok(moq_varint_size(MOQ_VARINT_MAX) == 8, "size(2^62-1) = 8");

    /* 逐档往返。 */
    {
        const uint64_t vals[] = { 0, 1, 63, 64, 16383, 16384,
                                  1073741823, 1073741824, MOQ_VARINT_MAX };
        for (size_t i = 0; i < sizeof(vals)/sizeof(vals[0]); i++) {
            MoqBuf b; moq_buf_init(&b);
            size_t off = 0; uint64_t back = 0;
            ok(moq_varint_write(&b, vals[i]) == 0 &&
               moq_varint_read(b.buf, b.len, &off, &back) == 0 &&
               back == vals[i] && off == b.len,
               "round trip");
            moq_buf_free(&b);
        }
    }

    /* 超上限必须拒绝，且**不写半截**。 */
    { MoqBuf b; moq_buf_init(&b);
      ok(moq_varint_write(&b, UINT64_C(1) << 62) < 0, "2^62 rejected");
      ok(b.len == 0, "rejected write leaves nothing behind");
      moq_buf_free(&b); }

    /* 非最小编码是**合法**的：`40 00` 是 0 的两字节形态（draft-20 §1.4.1
     * 明文 "do not need to be encoded using the minimum number of bytes"，
     * Table 2 还拿 0x8025 这种非最小形态当合法样例）。
     * 解码侧不能因为非最小就断连 —— 只是发端自己只发最短（同一值只有一种
     * 字节形态），收了非最小的照样得认。 */
    { const uint8_t nonmin[] = { 0x40, 0x00 };   /* 2-byte form of 0 */
      size_t off = 0; uint64_t v = 99;
      ok(moq_varint_read(nonmin, sizeof(nonmin), &off, &v) == 0 && v == 0 &&
         off == 2, "non-minimal varint 0x4000 decodes to 0"); }

    /* 不够 vs 坏数据的区分 —— 流式解析全靠这个。 */
    { const uint8_t shortv[] = { 0x40 };        /* 声明 2 字节但只有 1 字节 */
      size_t off = 0; uint64_t v = 0;
      ok(moq_varint_read(shortv, sizeof(shortv), &off, &v) == -1 &&
         off == 0, "truncated varint -> -1, offset not advanced"); }

    { size_t off = 0; uint64_t v = 0;
      ok(moq_varint_read(NULL, 0, &off, &v) == -1 && off == 0,
         "empty buffer -> -1 (streaming norm, not an error)"); }
}

static void test_zigzag(void) {
    printf("[2] zigzag\n");

    { uint64_t z = 0;
      ok(moq_zigzag_encode(0, &z) == 0 && z == 0, "zigzag(0) = 0");
      ok(moq_zigzag_encode(-1, &z) == 0 && z == 1, "zigzag(-1) = 1");
      ok(moq_zigzag_encode(1, &z) == 0 && z == 2, "zigzag(1) = 2");
      ok(moq_zigzag_encode(-21, &z) == 0 && z == 41, "zigzag(-21) = 41");
      ok(moq_zigzag_encode(10062000, &z) == 0 && z == 20124000,
         "zigzag(10062000) = 20124000"); }

    ok(moq_zigzag_decode(0) == 0, "unzigzag(0) = 0");
    ok(moq_zigzag_decode(1) == -1, "unzigzag(1) = -1");
    ok(moq_zigzag_decode(41) == -21, "unzigzag(41) = -21");
    ok(moq_zigzag_decode(20124000) == 10062000, "unzigzag(20124000)");

    /* 域是**非对称**的 [-2^61, 2^61-1]：zigzag 后的值要塞进 62 位 varint。 */
    { uint64_t z = 0;
      ok(moq_zigzag_encode(MOQ_ZIGZAG_MIN, &z) == 0, "zigzag(min) ok");
      ok(moq_zigzag_encode(MOQ_ZIGZAG_MAX, &z) == 0, "zigzag(max) ok");
      ok(moq_zigzag_encode(MOQ_ZIGZAG_MAX + 1, &z) < 0, "zigzag(max+1) rejected");
      ok(moq_zigzag_encode(MOQ_ZIGZAG_MIN - 1, &z) < 0, "zigzag(min-1) rejected"); }
}

static void test_string(void) {
    printf("[3] (s) length-prefixed string\n");

    { MoqBuf b; moq_buf_init(&b); size_t off = 0; MoqStr s;
      ok(moq_str_write(&b, "hang") == 0, "write string hang");
      BYTES_EQ(&b, 0x04, 'h', 'a', 'n', 'g');
      ok(moq_str_read(b.buf, b.len, &off, &s) == 0 && s.len == 4 &&
         memcmp(s.p, "hang", 4) == 0 && off == b.len, "read string back");
      moq_buf_free(&b); }

    /* 空串是合法且常见的（广播路由恰好等于请求前缀时 suffix 就是空串）。 */
    { MoqBuf b; moq_buf_init(&b); size_t off = 0; MoqStr s;
      ok(moq_str_write(&b, "") == 0, "write empty string");
      BYTES_EQ(&b, 0x00);
      ok(moq_str_read(b.buf, b.len, &off, &s) == 0 && s.len == 0,
         "read empty string back");
      moq_buf_free(&b); }

    /* 声明长度超过剩余字节是**坏数据**（-2），不是「还不够」（-1）——
     * 这个区分决定是 reset 流还是继续等。 */
    { const uint8_t bad[] = { 0x08, 'a', 'b' };
      size_t off = 0; MoqStr s;
      ok(moq_str_read(bad, sizeof(bad), &off, &s) == -2,
         "declared length over remaining -> -2"); }
}

static void test_message_frame(void) {
    printf("[4] message framing\n");

    /* typed=false：[Length][body]。 */
    { MoqBuf b; moq_buf_init(&b); size_t off = 0, blen = 0;
      ok(moq_msg_build(&b, 0, 0, "AB", 2) == 0, "build untyped");
      BYTES_EQ(&b, 0x02, 'A', 'B');
      ok(moq_msg_read(b.buf, b.len, &off, &blen) == MOQ_MSG_OK && blen == 2 &&
         off == 1, "read untyped header");
      moq_buf_free(&b); }

    /* typed=true：[Type][Length][body]。实测样例 ANNOUNCE_END{id=42} =
     * 01 01 2A —— 长度**不含** Type。 */
    { MoqBuf b; moq_buf_init(&b);
      const uint8_t body[] = { 42 };
      ok(moq_msg_build(&b, 1, 0x1, body, sizeof(body)) == 0, "build typed");
      BYTES_EQ(&b, 0x01, 0x01, 0x2A);
      moq_buf_free(&b); }

    /* 还没到齐 → NEED_MORE，且 off 不动（调用方要留着缓冲等下一次回调）。 */
    { const uint8_t part[] = { 0x05, 'a' };
      size_t off = 0, blen = 0;
      ok(moq_msg_read(part, sizeof(part), &off, &blen) == MOQ_MSG_NEED_MORE &&
         off == 0, "partial body -> NEED_MORE, offset unchanged"); }

    /* 声明的长度超过上限 → TOO_LARGE（违规），不必等字节到齐。 */
    { MoqBuf b; moq_buf_init(&b); size_t off = 0, blen = 0;
      ok(moq_varint_write(&b, MOQ_MAX_MESSAGE_SIZE + 1) == 0, "write huge length");
      ok(moq_msg_read(b.buf, b.len, &off, &blen) == MOQ_MSG_TOO_LARGE,
         "over-max length -> TOO_LARGE");
      moq_buf_free(&b); }
}

/* ============================================================
 * 整条流的形状演练：STREAM_TYPE + GROUP 头 + 两帧，逐条切完不留尾巴。
 * ============================================================ */

static void test_group_stream_shape(void) {
    printf("[5] group stream shape\n");

    MoqBuf s; moq_buf_init(&s);
    ok(moq_put_stream_type(&s, 0x0) == 0, "write STREAM_TYPE = Group");
    BYTES_EQ(&s, 0x00);

    MoqBuf hdr; moq_buf_init(&hdr);
    { MoqBuf body; moq_buf_init(&body);
      ok(moq_varint_write(&body, 1) == 0,  "group sub id");
      ok(moq_varint_write(&body, 0) == 0,  "group seq");
      ok(moq_varint_write(&body, 0) == 0,  "group frame_start");
      ok(moq_msg_build(&hdr, 0, 0, body.buf, body.len) == 0, "group header");
      moq_buf_free(&body); }
    BYTES_EQ(&hdr, 0x03, 0x01, 0x00, 0x00);   /* 与 C++ 侧同一条向量 */
    ok(moq_buf_append(&s, hdr.buf, hdr.len) == 0, "append header");

    /* 两帧：差 0 / 差 -21（音频帧间差的量级）。 */
    { MoqBuf f; moq_buf_init(&f);
      uint64_t z = 0;
      const uint8_t p0[2] = { 0xAA, 0xBB };
      ok(moq_zigzag_encode(0, &z) == 0 && moq_varint_write(&f, z) == 0 &&
         moq_varint_write(&f, 2) == 0 && moq_buf_append(&f, p0, 2) == 0,
         "frame 0");
      ok(moq_buf_append(&s, f.buf, f.len) == 0, "append frame 0");
      moq_buf_clear(&f);
      ok(moq_zigzag_encode(-21, &z) == 0 && moq_varint_write(&f, z) == 0 &&
         moq_varint_write(&f, 0) == 0, "frame 1 (empty payload)");
      ok(moq_buf_append(&s, f.buf, f.len) == 0, "append frame 1");
      moq_buf_free(&f); }

    /* 现在按收侧的读法把这条流走一遍：必须正好切完。 */
    { size_t off = 0; uint64_t st = 0;
      ok(moq_varint_read(s.buf, s.len, &off, &st) == 0 && st == 0x0,
         "stream type is Group");

      size_t blen = 0;
      ok(moq_msg_read(s.buf, s.len, &off, &blen) == MOQ_MSG_OK, "group header fits");
      size_t p = off, end = off + blen;
      uint64_t sub = 0, seq = 0, fs = 0;
      ok(moq_varint_read(s.buf, end, &p, &sub) == 0 && sub == 1,   "sub id = 1");
      ok(moq_varint_read(s.buf, end, &p, &seq) == 0 && seq == 0,   "seq = 0");
      ok(moq_varint_read(s.buf, end, &p, &fs)  == 0 && fs  == 0,   "frame_start = 0");
      ok(p == end, "group header body exactly consumed");
      off = end;

      /* 帧 0 */
      uint64_t zz = 0;
      ok(moq_varint_read(s.buf, s.len, &off, &zz) == 0 &&
         moq_zigzag_decode(zz) == 0, "frame 0 delta");
      ok(moq_msg_read(s.buf, s.len, &off, &blen) == MOQ_MSG_OK && blen == 2,
         "frame 0 payload len");
      ok(off + blen <= s.len && s.buf[off] == 0xAA, "frame 0 payload byte");
      off += blen;

      /* 帧 1 */
      ok(moq_varint_read(s.buf, s.len, &off, &zz) == 0 &&
         moq_zigzag_decode(zz) == -21, "frame 1 delta");
      ok(moq_msg_read(s.buf, s.len, &off, &blen) == MOQ_MSG_OK && blen == 0,
         "frame 1 empty payload");
      off += blen;

      ok(off == s.len, "whole group stream consumed, no trailing bytes"); }

    moq_buf_free(&hdr);
    moq_buf_free(&s);
}

/* ============================================================
 * 控制消息的形状（订阅端会**发**的那三条 + 会**收**的那些）
 * ============================================================ */

static void test_control_messages(void) {
    printf("[6] control messages\n");

    /* SETUP 流：STREAM_TYPE(0x1) + [Length=1][Parameter Count=0]。
     * 消息体里没有 SETUP 判别符 —— 类型就是 STREAM_TYPE。
     * ⚠️ Parameter Count 是**消息体的一部分**（所以 Length=1），不是跟在
     * 消息后面另起的一段：先建空体再补一个 00 会写出 `01 00 00`（长度=0），
     * 读侧按长度切就只切到 0 字节、把那个 00 漏给下一个消息。 */
    { MoqBuf b; moq_buf_init(&b);
      MoqBuf body; moq_buf_init(&body);
      ok(moq_varint_write(&body, 0) == 0, "parameter count = 0");
      ok(moq_put_stream_type(&b, 0x1) == 0, "setup stream type");
      ok(moq_msg_build(&b, 0, 0, body.buf, body.len) == 0, "setup message");
      BYTES_EQ(&b, 0x01, 0x01, 0x00);
      moq_buf_free(&body);
      moq_buf_free(&b); }

    /* ANNOUNCE_REQUEST：普通消息（无 Type），body 只有一个 (s) 前缀。 */
    { MoqBuf b; moq_buf_init(&b);
      MoqBuf body; moq_buf_init(&body);
      ok(moq_str_write(&body, "") == 0, "announce prefix \"\"");
      ok(moq_msg_build(&b, 0, 0, body.buf, body.len) == 0, "announce request");
      BYTES_EQ(&b, 0x01, 0x00);            /* [len=1][prefix ""] */
      moq_buf_free(&body);
      moq_buf_free(&b); }

    /* SUBSCRIBE：Lite06 上**没有 Group Order 那个字节**（01~05 专有），
     * Priority 是**单字节 u8**（不是 varint），Frame Start/End 是 06 新增。
     * 多发/少发一个字节都会让后面每个字段错位，而且不报错。 */
    { MoqBuf body; moq_buf_init(&body);
      ok(moq_varint_write(&body, 7) == 0,                          "sub id");
      ok(moq_str_write(&body, "ford_100.hang") == 0,               "broadcast");
      ok(moq_str_write(&body, "catalog.json") == 0,                "track");
      ok(moq_buf_append(&body, "\x00", 1) == 0,                    "priority = 0 (u8)");
      ok(moq_varint_write(&body, 5000) == 0,                       "max age 5000");
      ok(moq_varint_write(&body, 0) == 0,                          "group start 0");
      ok(moq_varint_write(&body, 0) == 0,                          "group end 0");
      ok(moq_varint_write(&body, 0) == 0,                          "frame start 0");
      ok(moq_varint_write(&body, 0) == 0,                          "frame end 0");

      /* 走一遍解码，确认字段**恰好**是这九项，既不剩也不缺。 */
      size_t p = 0;
      uint64_t v = 0; MoqStr s;
      ok(moq_varint_read(body.buf, body.len, &p, &v) == 0 && v == 7, "id back");
      ok(moq_str_read(body.buf, body.len, &p, &s) == 0 && s.len == 13 &&
         memcmp(s.p, "ford_100.hang", 13) == 0, "broadcast back");
      ok(moq_str_read(body.buf, body.len, &p, &s) == 0 && s.len == 12 &&
         memcmp(s.p, "catalog.json", 12) == 0, "track back");
      ok(p < body.len && body.buf[p] == 0x00, "priority is one byte 0x00");
      p += 1;
      ok(moq_varint_read(body.buf, body.len, &p, &v) == 0 && v == 5000, "max age back");
      ok(moq_varint_read(body.buf, body.len, &p, &v) == 0 && v == 0, "group start");
      ok(moq_varint_read(body.buf, body.len, &p, &v) == 0 && v == 0, "group end");
      ok(moq_varint_read(body.buf, body.len, &p, &v) == 0 && v == 0, "frame start");
      ok(moq_varint_read(body.buf, body.len, &p, &v) == 0 && v == 0, "frame end");
      ok(p == body.len, "SUBSCRIBE body exactly consumed (no group-order byte)");
      moq_buf_free(&body); }

    /* 收侧：ANNOUNCE_OK{hop,active} —— 顺序是 **Hop ID 在前、Active Count 在后**。 */
    { const uint8_t ann_ok[] = { 0x02, 0x2A, 0x01 };   /* [len=2][hop=42][active=1] */
      size_t off = 0, blen = 0, p = 0;
      uint64_t hop = 0, active = 0;
      ok(moq_msg_read(ann_ok, sizeof(ann_ok), &off, &blen) == MOQ_MSG_OK,
         "ANNOUNCE_OK readable");
      p = off;                          /* 体从长度字段之后开始 */
      ok(moq_varint_read(ann_ok, off + blen, &p, &hop) == 0 && hop == 42,
         "ANNOUNCE_OK hop id first");
      ok(moq_varint_read(ann_ok, off + blen, &p, &active) == 0 && active == 1,
         "ANNOUNCE_OK active count second");
      ok(p == off + blen, "ANNOUNCE_OK body exactly consumed"); }

    /* 收侧：ANNOUNCE_START body — suffix(s) hops count(i) hop ids… warm(i) cold(i)。
     * ⚠️ 代价顺序是 warm 在前、cold 在后；参考实现的测试特意用不对称的
     * (12, 30) 防这个写反。 */
    { const uint8_t body[] = { 0x02, 'h', 'i', 0x01, 0x2A, 0x0C, 0x1E };
      size_t p = 0; MoqStr suffix;
      uint64_t hops = 0, hop = 0, warm = 0, cold = 0;
      ok(moq_str_read(body, sizeof(body), &p, &suffix) == 0 &&
         suffix.len == 2 && memcmp(suffix.p, "hi", 2) == 0,
         "ANNOUNCE_START suffix");
      ok(moq_varint_read(body, sizeof(body), &p, &hops) == 0 && hops == 1,
         "hop count");
      ok(moq_varint_read(body, sizeof(body), &p, &hop) == 0 && hop == 42,
         "hop id");
      ok(moq_varint_read(body, sizeof(body), &p, &warm) == 0 && warm == 12,
         "warm cost first");
      ok(moq_varint_read(body, sizeof(body), &p, &cold) == 0 && cold == 30,
         "cold cost second");
      ok(p == sizeof(body), "ANNOUNCE_START body exactly consumed"); }

    /* 收侧：SUBSCRIBE_START{group} —— -06 上接受是**隐式**的，第一条应答就是
     * 这条，没有 SUBSCRIBE_OK。线上形状 [Type=0x0][Length][group]。
     * ⚠️ Length 是 body 的字节数：group=1000 是 2 字节 varint（43 E8），
     * 所以 Length=02 而不是 03 —— 长度字段自己**不算**在内。 */
    { const uint8_t sub_start[] = { 0x00, 0x02, 0x43, 0xE8 };
      size_t off = 0, blen = 0, p = 0; uint64_t type = 0, group = 0;
      /* 带判别符的消息：调用方先读走 Type，再让 moq_msg_read 从它之后接着切。 */
      ok(moq_varint_read(sub_start, sizeof(sub_start), &off, &type) == 0 &&
         type == 0x0, "SUBSCRIBE_START type = 0x0 (accept is implicit)");
      ok(moq_msg_read(sub_start, sizeof(sub_start), &off, &blen) == MOQ_MSG_OK &&
         blen == 2, "SUBSCRIBE_START body readable, length = 2");
      p = off;
      ok(moq_varint_read(sub_start, off + blen, &p, &group) == 0 && group == 1000 &&
         p == off + blen, "SUBSCRIBE_START group = 1000"); }
}

/* ============================================================
 * 发布端（serve）方向的编码 —— 与上面那套是**反方向**：我们是发布端，对端来
 * 订阅。向量与 moqlivecast/moq/moq_lite_wire_demo.cpp 的同名断言一致。
 * ============================================================ */

static void test_serve_messages(void) {
    printf("[7] serve (publish) messages\n");

    /* ANNOUNCE_OK：origin 在前、active 在后 */
    { MoqBuf b; moq_buf_init(&b);
      ok(moq_wire_announce_ok(&b, 0, 1) == 0, "build ANNOUNCE_OK active=1");
      BYTES_EQ(&b, 0x02, 0x00, 0x01);
      moq_buf_free(&b); }

    { MoqBuf b; moq_buf_init(&b);
      ok(moq_wire_announce_ok(&b, 0, 0) == 0, "build ANNOUNCE_OK active=0");
      BYTES_EQ(&b, 0x02, 0x00, 0x00);
      moq_buf_free(&b); }

    /* ANNOUNCE_START：suffix 空（最常见）+ hops 空链 + cost {0,0} */
    { MoqBuf b; moq_buf_init(&b);
      ok(moq_wire_announce_start(&b, "", 0, 0) == 0, "build ANNOUNCE_START \"\"");
      BYTES_EQ(&b, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00);
      moq_buf_free(&b); }

    { MoqBuf b; moq_buf_init(&b);
      ok(moq_wire_announce_start(&b, "hang", 0, 0) == 0, "build ANNOUNCE_START hang");
      BYTES_EQ(&b, 0x00, 0x08, 0x04, 'h', 'a', 'n', 'g', 0x00, 0x00, 0x00);
      moq_buf_free(&b); }

    /* SUBSCRIBE_START：-06 上接受是隐式的，没有 SUBSCRIBE_OK */
    { MoqBuf b; moq_buf_init(&b);
      ok(moq_wire_subscribe_start(&b, 0) == 0, "build SUBSCRIBE_START group=0");
      BYTES_EQ(&b, 0x00, 0x01, 0x00);
      moq_buf_free(&b); }

    { MoqBuf b; moq_buf_init(&b);
      ok(moq_wire_subscribe_start(&b, 1000) == 0, "build SUBSCRIBE_START 1000");
      BYTES_EQ(&b, 0x00, 0x02, 0x43, 0xE8);
      moq_buf_free(&b); }

    { MoqBuf b; moq_buf_init(&b);
      ok(moq_wire_subscribe_end(&b, 1000) == 0, "build SUBSCRIBE_END 1000");
      BYTES_EQ(&b, 0x01, 0x02, 0x43, 0xE8);
      moq_buf_free(&b); }

    /* TRACK_INFO：**没有 group-order 字节**、priority 是裸 u8、timescale 是
     * 1000000（> 16383 所以只能走 4 字节形式）。 */
    { MoqBuf b; moq_buf_init(&b);
      ok(moq_wire_track_info(&b, 0, 5000, 1000000) == 0, "build TRACK_INFO");
      BYTES_EQ(&b, 0x07, 0x00, 0x53, 0x88, 0x80, 0x0F, 0x42, 0x40);
      moq_buf_free(&b); }

    /* priority=200 在 u8 下是单字节 C8；若误写成 varint 会变成 40 C8（两字节）
     * 且长度字段跟着错 —— 这条专门盯它。 */
    { MoqBuf b; moq_buf_init(&b);
      ok(moq_wire_track_info(&b, 200, 0, 1000) == 0, "build TRACK_INFO prio=200");
      BYTES_EQ(&b, 0x04, 0xC8, 0x00, 0x43, 0xE8);
      moq_buf_free(&b); }

    /* GROUP 头：⚠️ 调用方要先写 STREAM_TYPE，这里只断言消息本体。 */
    { MoqBuf b; moq_buf_init(&b);
      ok(moq_wire_group_header(&b, 1, 0, 0) == 0, "build GROUP header");
      BYTES_EQ(&b, 0x03, 0x01, 0x00, 0x00);
      moq_buf_free(&b); }

    { MoqBuf b; moq_buf_init(&b);
      ok(moq_wire_group_header(&b, 1, 1000, 7) == 0, "build GROUP 1000/7");
      BYTES_EQ(&b, 0x04, 0x01, 0x43, 0xE8, 0x07);
      moq_buf_free(&b); }

    /* FRAME：差是 zigzag，长度只覆盖 payload。 */
    { MoqBuf b; moq_buf_init(&b);
      ok(moq_wire_frame(&b, 0, NULL, 0) == 0, "build FRAME delta=0 empty");
      BYTES_EQ(&b, 0x00, 0x00);
      moq_buf_free(&b); }

    { MoqBuf b; moq_buf_init(&b);
      ok(moq_wire_frame(&b, -21, NULL, 0) == 0, "build FRAME delta=-21");
      BYTES_EQ(&b, 0x29, 0x00);
      moq_buf_free(&b); }

    { MoqBuf b; moq_buf_init(&b);
      const uint8_t pl[3] = {0xAA, 0xBB, 0xCC};
      ok(moq_wire_frame(&b, 10062000, pl, sizeof(pl)) == 0, "build FRAME big delta");
      BYTES_EQ(&b, 0x81, 0x33, 0x11, 0x60, 0x03, 0xAA, 0xBB, 0xCC);
      moq_buf_free(&b); }
}

int main(void) {
    printf("moq-lite wire self-test (C)\n\n");

    test_varint();
    test_zigzag();
    test_string();
    test_message_frame();
    test_group_stream_shape();
    test_control_messages();
    test_serve_messages();

    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
