#ifndef MOQ_LITE_WIRE_HPP
#define MOQ_LITE_WIRE_HPP

#include "quic_packet.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace cpp_streamer {

/* moq-lite 的线编码助手。
 *
 * ⚠️ 与 moq/moq_varint.hpp 的差别是**实质性**的，不是风格问题：
 *   moq-lite 的 (i) 是 **QUIC varint（RFC 9000 §16）**，上限 2^62-1；
 *   moq_varint.hpp 那套是 MOQT 的 leading-ones vi64。
 * 两者**只有 0~63 一致** —— 64 以上长度或前缀就分岔（详见
 * docs/moq-transport-gap.md §12.15）。
 *
 * 证据（只读参考实现，不由规范性引用反推）：
 *   rs/moq-net/src/coding/varint.rs —— 文件头 "Based on quinn-proto"，
 *     "suitable for encoding as QUIC variable-length integer"，MAX = (1<<62)-1
 *   调用点 rs/moq-net/src/lite/publisher.rs:1934、lite/subscriber.rs:891
 *
 * 两套编码在本仓**并存是故意的**：draft-20 那条线（moq_push.cpp / 浏览器
 * WebTransport）继续用 moq_varint.hpp，这里这套只服务 moq-lite。不要合并。
 *
 * 底层复用 QUIC 传输自己的 quic_varint_encode/decode/len（quic/quic_packet.c）：
 * moqlivecast/CMakeLists.txt 已把 quic/ 放进 include path，并把 quic_packet.c
 * 编进同一个可执行文件，所以这里可以直接调，不必另写一份。 */

/* (i) 的上限：RFC 9000 §16 的 8 字节形式只有 62 个值位。 */
constexpr uint64_t kMlVarintMax = (uint64_t{1} << 62) - 1;

/* (i) —— QUIC varint。val 超出上限时返回 false 且 out 不变。
 *
 * ⚠️ 这里必须自己判上限：quic_varint_encode 对 ≥ 2^62 的值**不报错**，它按
 * 8 字节形式写、而首字节的高 2 位被 `| 0xc0` 覆盖掉 —— 于是**静默截断成另一个
 * 数**（实测 UINT64_MAX 能编出去，解回来并不是它）。QUIC 传输自己撞不到这种
 * 值，所以那边不设防是可以的；协议层会碰到，守卫就放在这里。
 * 同理解码侧不用判：线上来的 8 字节形式天然只承载 62 位。 */
inline bool MlWriteVarint(std::vector<uint8_t> &out, uint64_t val) {
    if (val > kMlVarintMax) return false;
    uint8_t tmp[8];
    const int n = quic_varint_encode(tmp, sizeof(tmp), val);
    if (n < 0) return false;
    out.insert(out.end(), tmp, tmp + n);
    return true;
}

/* 追加形态，忽略失败。调用点都是常量或已校验过的值；要检查就用 MlWriteVarint。 */
inline void MlAppendVarint(std::vector<uint8_t> &out, uint64_t val) {
    (void)MlWriteVarint(out, val);
}

/* (i) 解码。off 既是入参也是出参：只在成功时前进（失败时留给调用方判断
 * "还没到齐"还是"真的坏了" —— moq-lite 是流式解析，这个区分很关键）。
 *
 * ⚠️ `off >= len` 必须在这里就返回，不能落到下面的 quic_varint_decode：
 * 那个函数对 len==0 是安全的（先判长度再看字节），但会**打一条 ERROR 日志**。
 * 流式解析里"缓冲空了"是每次回调都会遇到的正常状态，让它去刷错误日志会把
 * 真正的错误埋掉（也是 R13 日志量的来源之一）。 */
inline bool MlReadVarint(const uint8_t *p, size_t len, size_t &off, uint64_t &out) {
    if (off >= len) return false;
    const int n = quic_varint_decode(p + off, len - off, &out);
    if (n < 0) return false;
    off += static_cast<size_t>(n);
    return true;
}

/* 只要跳过，不要值（GROUP 头的 Frame Start 这类用不上但必须消费的字段）。 */
inline bool MlSkipVarint(const uint8_t *p, size_t len, size_t &off) {
    uint64_t v = 0;
    return MlReadVarint(p, len, off, v);
}

inline size_t MlVarintLen(uint64_t val) { return quic_varint_len(val); }

/* ============================================================
 * 消息框 —— 每条 moq-lite 消息都套着 Message Length
 * ============================================================ */

/* 长度字段的上限。rs/moq-net/src/lite/message.rs:9 MAX_MESSAGE_SIZE = 64 MiB。
 * 超限的长度字段本身就是违规，不必等对端把字节发完（也就不会被它拖死）。 */
constexpr uint64_t kMlMaxMessageSize = 64ull * 1024 * 1024;

/* 消息框：`[Message Length(i)][body]`。
 *
 * ⚠️ 长度**只覆盖 body**：
 *   - 不含长度字段自己（-06 §7.1）；
 *   - 带类型判别符的消息（ANNOUNCE_START 那三种、SUBSCRIBE 的三种应答），
 *     Type 在长度**之外**、之前 —— 即 `[Type(i)][Length(i)][body]`，
 *     Length 不含 Type。实测样例：ANNOUNCE_END{id=42} = `01 01 2A`。
 *
 * 三态是刻意的：流式解析里「还没到齐」与「真坏了」的处置完全不同 ——
 * 前者要留着缓冲等下一次回调，后者要 reset 流。 */
enum MlMsgStatus {
    kMlMsgOk = 0,
    kMlMsgNeedMore,
    kMlMsgTooLarge,
};

/* 只在 kMlMsgOk 时前进 off（到 body 起点）。 */
inline MlMsgStatus MlReadMessage(const uint8_t *p, size_t len, size_t &off,
                                 size_t &body_len) {
    size_t o = off;
    uint64_t n = 0;
    if (!MlReadVarint(p, len, o, n)) return kMlMsgNeedMore;
    if (n > kMlMaxMessageSize) return kMlMsgTooLarge;
    if (n > len - o) return kMlMsgNeedMore;   /* o <= len 由上一步保证 */
    body_len = static_cast<size_t>(n);
    off = o;
    return kMlMsgOk;
}

/* 编码侧：把 [Type?][Length][body] 拼成一个可直接送 StreamSend 的缓冲。
 * typed=false 时就是普通的 [Length][body]（ANNOUNCE_REQUEST / ANNOUNCE_OK）。 */
inline bool MlBuildMessage(std::vector<uint8_t> &out, bool typed, uint64_t type,
                           const std::vector<uint8_t> &body) {
    if (typed && !MlWriteVarint(out, type)) return false;
    if (!MlWriteVarint(out, body.size())) return false;
    out.insert(out.end(), body.begin(), body.end());
    return true;
}

/* ============================================================
 * zigzag —— FRAME 的时间戳差是有符号的
 * ============================================================ */

/* 有效域 [-2^61, 2^61-1]（**非对称**），照抄参考实现：
 *   rs/moq-net/src/coding/varint.rs:62-68  from_zigzag
 *     const RANGE: i64 = 1 << 61;
 *     if signed < -RANGE || signed >= RANGE { BoundsExceeded }
 * 理由是 zigzag 后的值要塞进 62 位 varint。 */
constexpr int64_t kMlZigzagMin = -(int64_t{1} << 61);
constexpr int64_t kMlZigzagMax = (int64_t{1} << 61) - 1;

/* unsigned = (signed << 1) ^ (signed >> 63)。越界返回 false。 */
inline bool MlZigzagEncode(int64_t v, uint64_t &out) {
    if (v < kMlZigzagMin || v > kMlZigzagMax) return false;
    /* 先转无符号再左移，避免有符号左移溢出（UB）；
     * >> 63 要的是算术右移（负数全 1），gcc/clang 都是。 */
    out = (static_cast<uint64_t>(v) << 1) ^ static_cast<uint64_t>(v >> 63);
    return true;
}

/* signed = (unsigned >> 1) ^ -(unsigned & 1)。解码侧不设范围检查 ——
 * 线上来的值本身就是编码侧校验过的，这里只管还原。 */
inline int64_t MlZigzagDecode(uint64_t v) {
    return static_cast<int64_t>(v >> 1) ^ -static_cast<int64_t>(v & 1);
}

/* ============================================================
 * (s) —— 长度前缀 UTF-8 串，长度本身也是 (i)
 * ============================================================ */

inline bool MlWriteString(std::vector<uint8_t> &out, const std::string &s) {
    if (!MlWriteVarint(out, s.size())) return false;
    out.insert(out.end(), s.begin(), s.end());
    return true;
}

inline bool MlReadString(const uint8_t *p, size_t end, size_t &off, std::string &out) {
    uint64_t n = 0;
    if (!MlReadVarint(p, end, off, n)) return false;
    if (n > end - off) return false;
    out.assign(reinterpret_cast<const char *>(p + off), static_cast<size_t>(n));
    off += static_cast<size_t>(n);
    return true;
}

/* ============================================================
 * 编码侧 —— 只服务「对端是 subscriber」的发布端角色
 *
 * 解码侧那套（M1–M3，订阅者角色）在上面，两边共享 MlWriteVarint /
 * MlBuildMessage / MlZigzagEncode。这里每个函数只写**我们实际会发的形状**，
 * 不做通用化：参考实现里那些带 hops / cost / frame bounds 的变体我们一个都
 * 用不上，多一个用不上的参数就多一处能传错的地方。
 *
 * 全部字段顺序与类型都已对着 rs/moq-net/src/lite/ 逐字节核过，见各自的注释。
 * ⚠️ 唯一的防线是 moq_lite_wire_demo.cpp 里的字节级断言 —— 这些函数没有
 * 其它调用者，写错了不会编译报错、只会在对端静默错位。 */

/* TRACK_INFO —— Track 流（bidi 0x6）上发布端的**唯一**一条应答，写完就 FIN。
 *   [size][priority u8][max_age (i) 毫秒][timescale (i)]
 *
 * ⚠️ **没有 group-order 字节**。那个字段只有 Lite01–05 有
 * （rs/moq-net/src/lite/version.rs:164-169，Lite06/07 返回 false），
 * `pad_group_order` 在新版本上是空操作（lite/subscribe.rs:113-126）。
 * 参考实现自带的跨语言断言 [06 00 00 53 88 43 E8] 是 **Lite05** 的：
 * size=6 + priority + group-order + max_age + timescale —— 照它抄会多一个字节。
 *
 * ⚠️ priority 是 **u8 而不是 varint**（lite/track.rs:66 `pub priority: u8`，
 * 且 `u8::encode` 恒写 1 字节）。timescale 走 QUIC varint 的 4/8 字节形式，
 * 因为 1000000 > 16383 —— 写成三字节是编不出来的（quic_packet.c:67-72
 * 只有 1/2/4/8 四种长度）。 */
inline bool MlBuildTrackInfo(std::vector<uint8_t> &out, uint8_t priority,
                             uint64_t max_age_ms, uint64_t timescale) {
    std::vector<uint8_t> body;
    body.reserve(12);
    body.push_back(priority);
    if (!MlWriteVarint(body, max_age_ms)) return false;
    if (!MlWriteVarint(body, timescale)) return false;
    /* typed=false：TRACK_INFO 是普通消息，线上没有类型判别符 */
    return MlBuildMessage(out, false, 0, body);
}

/* ANNOUNCE_OK —— announce 流（bidi 0x1）上发布端的第一条消息，无类型判别符。
 *   [size][origin (i)][active (i)]
 * 紧跟着就是 active 条 ANNOUNCE_START（rs/moq-net/src/lite/publisher.rs:748-758
 * 的顺序）。active == 0 合法，且**流要保持打开**继续等后续 ANNOUNCE_START。 */
inline bool MlBuildAnnounceOk(std::vector<uint8_t> &out, uint64_t origin,
                              uint64_t active) {
    std::vector<uint8_t> body;
    body.reserve(16);
    if (!MlWriteVarint(body, origin)) return false;
    if (!MlWriteVarint(body, active)) return false;
    return MlBuildMessage(out, false, 0, body);
}

/* ANNOUNCE_START —— typed(0x0)，跟在 ANNOUNCE_OK 后面。
 *   [type=0x0][size][suffix (s)][hops count (i)][hop id (i)…][warm (i)][cold (i)]
 *
 * suffix 是**相对订阅端请求 prefix** 的路径（publisher.rs:543-574 的 rebase）：
 * 广播的路由恰好等于 prefix 时是空串 —— 也就是最常见的那种。
 *
 * hops 我们**恒发空链**（一个 `00` 计数）：本端就是内容源头，
 * 既没有上游也没有跳数可报（model/origin.rs:320-332 的 Hops 编码 =
 * 计数 + 每个 id 一个 varint）。
 *
 * ⚠️ cost 在 Lite06+ 上**要发**（version.rs:214-220 has_route_cost），
 * Lite01–05 才不发。发 {0,0} = 「源头，取它不要钱」，这与 Cost::UNKNOWN
 * 的 {warm:0, cold:2^62-1} 不同后者是「线上没地方放 cost」时的兜底值；
 * 更不能用 Cost::MAX/DRAIN —— 那是「本连接正在下线，请优先选别人」
 * （model/origin.rs:404/412）。 */
inline bool MlAppendAnnounceStart(std::vector<uint8_t> &out,
                                  const std::string &suffix,
                                  uint64_t warm, uint64_t cold) {
    std::vector<uint8_t> body;
    body.reserve(suffix.size() + 24);
    if (!MlWriteString(body, suffix)) return false;
    if (!MlWriteVarint(body, 0)) return false;   /* hops count = 0 */
    if (!MlWriteVarint(body, warm)) return false;
    if (!MlWriteVarint(body, cold)) return false;
    return MlBuildMessage(out, true, /*ANNOUNCE_START=*/0x0, body);
}

/* ANNOUNCE_END —— typed(0x1)，body 只有一个 announce id，线上恰好 3 字节
 * （实测样例 id=42 是 `01 01 2A`）。live 流**不发**这个。 */
inline bool MlAppendAnnounceEnd(std::vector<uint8_t> &out, uint64_t id) {
    std::vector<uint8_t> body;
    if (!MlWriteVarint(body, id)) return false;
    return MlBuildMessage(out, true, /*ANNOUNCE_END=*/0x1, body);
}

/* SUBSCRIBE_START —— typed(0x0)，订阅流（bidi 0x2）上发布端的应答。
 *   [type=0x0][size][group (i)]
 *
 * ⚠️ **Lite06 上没有 SUBSCRIBE_OK**：接受订阅是**隐式**的，第一条应答就是
 * 这个 START（lite/subscribe.rs:490-518）。拒绝订阅 = 直接 reset 那条流，
 * 不是发一条消息。live 流不发 SUBSCRIBE_END。
 *
 * group 应当是**诚实的**：所以调用方要等第一个组真要写出去时才发这条，
 * 那时才知道起始组号（照参考实现的做法）。 */
inline bool MlBuildSubscribeStart(std::vector<uint8_t> &out, uint64_t group) {
    std::vector<uint8_t> body;
    if (!MlWriteVarint(body, group)) return false;
    return MlBuildMessage(out, true, /*SUBSCRIBE_START=*/0x0, body);
}

/* SUBSCRIBE_END —— typed(0x1)，同样只有一个 group。live 流不发。 */
inline bool MlBuildSubscribeEnd(std::vector<uint8_t> &out, uint64_t group) {
    std::vector<uint8_t> body;
    if (!MlWriteVarint(body, group)) return false;
    return MlBuildMessage(out, true, /*SUBSCRIBE_END=*/0x1, body);
}

/* GROUP 头 —— uni 流上的第一条消息。
 *   [size][subscribe id (i)][group sequence (i)][frame start (i)]
 *
 * ⚠️ 调用方要**先**把 STREAM_TYPE(=0x0，uni 的 Group) 写进同一个缓冲，
 * 再调这里 —— 流类型字节是流自己的头，不属于任何消息。收侧就是按这个顺序
 * 解的（见本仓 ParseGroupStream 的 "GROUP{ Message Length; Subscribe ID;
 * Group Sequence; Frame Start }"）。
 *
 * ⚠️ frame start 是 06 新增的第三个 varint，必须写。我们恒发 0（组一律从
 * 第 0 帧开始），但**不能省**：收侧会拿它当组内首帧的下标，少一个 varint
 * 后面第一个 FRAME 的时间戳差就整体错位，而且不报错。 */
inline bool MlBuildGroupHeader(std::vector<uint8_t> &out, uint64_t sub_id,
                               uint64_t seq, uint64_t frame_start) {
    std::vector<uint8_t> body;
    body.reserve(24);
    if (!MlWriteVarint(body, sub_id)) return false;
    if (!MlWriteVarint(body, seq)) return false;
    if (!MlWriteVarint(body, frame_start)) return false;
    return MlBuildMessage(out, false, 0, body);
}

/* FRAME —— 裸消息，一串背靠背。
 *   [Timestamp Delta (i, zigzag)][Message Length (i)][Payload]
 *
 * ts_delta 的单位是 **track timescale**，且是**组内累加**的差值：每组第一帧
 * 相对 0 取差，所以基线每组重置（publisher.rs:2439 的 prev_ts 初值就是 0）。
 *
 * ⚠️ Message Length 只覆盖 Payload（不含前面那个差），与 MlBuildMessage 的
 * 语义一致 —— 所以这里不用 MlBuildMessage：它不写前导的差。 */
inline bool MlAppendFrame(std::vector<uint8_t> &out, int64_t ts_delta,
                          const uint8_t *payload, size_t len) {
    uint64_t zz = 0;
    if (!MlZigzagEncode(ts_delta, zz)) return false;
    if (!MlWriteVarint(out, zz)) return false;
    if (!MlWriteVarint(out, static_cast<uint64_t>(len))) return false;
    out.insert(out.end(), payload, payload + len);
    return true;
}

} /* namespace cpp_streamer */

#endif /* MOQ_LITE_WIRE_HPP */
