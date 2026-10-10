#ifndef MOQ_LITE_WIRE_H
#define MOQ_LITE_WIRE_H

/* ============================================================
 * moq-lite-06 线编码 —— C 版
 *
 * 与 moqlivecast/moq/moq_lite_wire.hpp 是**同一套语义的两份实现**，那边是
 * C++ 服务端用的。改动任何一处都必须同步另一处 —— 两边的字节级断言是
 * moq_lite_wire_demo.cpp（C++）与 moq_lite_wire_test.c（本目录），
 * 向量一致就是防线上错位的唯一防线。
 *
 * ⚠️ 编码是 **QUIC varint（RFC 9000 §16）**，不是 MOQT 的 leading-ones vi64。
 * 两套**只有 0~63 一致**（64 以上长度分岔）。见
 * docs/moq-transport-gap.md §12.15 与 [moq-lite-direction-and-gap-doc]。
 *
 * 本文件刻意**不依赖** quic/ 的任何东西：这样编解码能单独编译自测，不必为了
 * 验一个 varint 去链接整个 QUIC 栈。代价是 varint 的解码器要自己写一份
 * （quic_varint_decode 在「字节还没到齐」时会打 ERROR 日志，而流式解析里
 * 那是每次回调都遇到的正常状态，用它会把真正的错误埋掉 —— 就是
 * moq_lite_wire.hpp:57 记的那个坑）。
 * ============================================================ */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── 可增长缓冲 ───────────────────────────────
 * 所有编码函数都往这里追加；分配失败返回 <0（调用方一律当作致命，不续写）。 */
typedef struct {
    uint8_t *buf;
    size_t   len;
    size_t   cap;
} MoqBuf;

void moq_buf_init(MoqBuf *b);
void moq_buf_free(MoqBuf *b);
void moq_buf_clear(MoqBuf *b);
int  moq_buf_reserve(MoqBuf *b, size_t extra);   /* 0 ok / -1 OOM */
int  moq_buf_append(MoqBuf *b, const void *p, size_t n);

/* 归零并释放内部数组（与 moq_buf_free 等价，名字上区分「重置再用」与「销毁」）。
 * 本 API 里所有会话对象析构时都用它，保证不留悬挂的 malloc。 */
void moq_buf_reset(MoqBuf *b);

/* ── (i) —— QUIC varint ─────────────────────── */

/* 上限：RFC 9000 §16 的 8 字节形式只有 62 个值位。
 * ⚠️ 必须自己判 —— quic_varint_encode 对 ≥ 2^62 的值**不报错**，它按 8 字节
 * 形式写、而首字节高 2 位被 `| 0xc0` 覆盖掉，于是**静默截断成另一个数**。
 * QUIC 传输自己撞不到这种值，协议层会碰到。 */
#define MOQ_VARINT_MAX ((UINT64_C(1) << 62) - 1)

size_t moq_varint_size(uint64_t v);               /* 1 / 2 / 4 / 8 */

/* 写一个 (i)。v 超上限返回 -1（**不写半截**，不留部分字节）。 */
int    moq_varint_write(MoqBuf *b, uint64_t v);

/* 读一个 (i)。off 既是入参也是出参：**只在成功时前进** —— 流式解析里
 * 「还没到齐」和「真坏了」的处置完全不同（前者留着缓冲等下次回调，后者
 * reset 流），调用方必须能区分。
 * 返回 0 成功 / -1 字节不够 / -2 解码器不认（超过 8 字节形态）。
 * ⚠️ 非最小编码是**合法**的（draft-20 §1.4.1 明文），这里不因非最小报错。 */
int    moq_varint_read(const uint8_t *p, size_t len, size_t *off, uint64_t *out);
int    moq_varint_skip(const uint8_t *p, size_t len, size_t *off);

/* ── zigzag —— FRAME 的时间戳差是有符号的 ───── */

/* 有效域 [-2^61, 2^61-1]（**非对称**）：zigzag 后的值要塞进 62 位 varint。
 * 照抄参考实现 rs/moq-net/src/coding/varint.rs:62-68。越界返回 -1。 */
#define MOQ_ZIGZAG_MIN (-(INT64_C(1) << 61))
#define MOQ_ZIGZAG_MAX  ((INT64_C(1) << 61) - 1)

int     moq_zigzag_encode(int64_t v, uint64_t *out);
int64_t moq_zigzag_decode(uint64_t v);

/* ── (s) —— 长度前缀 UTF-8 串，长度本身也是 (i) ── */

/* 不保证 NUL 结尾 —— 线上字符串可以带任意字节，不能假设它是 C 字符串。
 * 要当 C 字符串用就 moq_str_dup。 */
typedef struct {
    const uint8_t *p;
    size_t         len;
} MoqStr;

int     moq_str_write_n(MoqBuf *b, const void *p, size_t n);
int     moq_str_write(MoqBuf *b, const char *s);            /* s 为 NULL 视作空串 */
int     moq_str_read(const uint8_t *p, size_t end, size_t *off, MoqStr *out);
        /* 0 ok / -1 不够 / -2 长度越界（声明长度超过剩余字节，是坏数据而不是
         * 慢慢等能等到的 —— 流式下这两种必须分清） */

/* malloc 一份 NUL 结尾的副本。调用方负责 free。NULL = OOM。 */
char   *moq_str_dup(const MoqStr *s);

/* ── 消息框 —— 每条 moq-lite 消息都套着 Message Length ──
 *
 * 形状：`[Type?][Length][body]`，Length **只覆盖 body**
 *   - 不含长度字段自己（-06 §7.1）；
 *   - 带类型判别符的消息（ANNOUNCE_START 那三种、SUBSCRIBE 的三种应答），
 *     Type 在长度**之外**、之前。实测样例：ANNOUNCE_END{id=42} = `01 01 2A`。
 *
 * 上限 64 MiB —— 抄 rs/moq-net/src/lite/message.rs:9 MAX_MESSAGE_SIZE。
 * 超限的长度字段本身就是违规，不必等对端把字节发完（也就不会被它拖死）。 */
#define MOQ_MAX_MESSAGE_SIZE (UINT64_C(64) * 1024 * 1024)

typedef enum {
    MOQ_MSG_OK = 0,
    MOQ_MSG_NEED_MORE,     /* 还没到齐，留着缓冲等下次 */
    MOQ_MSG_TOO_LARGE,     /* 声明的长度超过上限 → 违规 */
} MoqMsgStatus;

/* 只在 MOQ_MSG_OK 时前进 off（到 body 起点）。 */
MoqMsgStatus moq_msg_read(const uint8_t *p, size_t len, size_t *off,
                          size_t *body_len);

/* 编码 [Type?][Length][body] 到 out。typed=0 时就是普通的 [Length][body]。
 * 0 ok / -1 OOM 或值越界。 */
int moq_msg_build(MoqBuf *out, int typed, uint64_t type,
                  const void *body, size_t body_len);

/* ── 发布端（serve）方向的消息 ─────────────────
 *
 * 与上面那套是**反方向**：我们是发布端，对端来订阅。全部与
 * moqlivecast/moq/moq_lite_wire.hpp 的对应函数逐字节一致（那是服务端角色的
 * 同一批函数）—— 两边的字节断言是 moq_lite_wire_demo.cpp / 本目录的
 * moq_lite_wire_test.c。 */

/* ANNOUNCE_OK —— announce 流上发布端的第一条消息，无类型判别符。
 *   [size][origin(i)][active(i)]
 * ⚠️ 字段顺序是 **origin 在前、active 在后**。origin 发 0 = Hop::UNKNOWN
 * （「没有身份」，我们不参与环路检测）；active 是随后要发几条 ANNOUNCE_START。
 * active == 0 合法，且**流要保持打开**继续等后续更新（不发 FIN）。 */
int moq_wire_announce_ok(MoqBuf *out, uint64_t origin, uint64_t active);

/* ANNOUNCE_START —— typed(0x0)，跟在 ANNOUNCE_OK 后面。
 *   [type=0x0][size][suffix(s)][hops count(i)=0][warm(i)][cold(i)]
 *
 * suffix 是**相对订阅端请求 prefix** 的路径（广播恰好等于请求前缀时是空串 ——
 * 最常见的那种）。hops 我们恒发空链（本端就是源头，既没有上游也没有跳数）。
 * ⚠️ cost 在 Lite06+ 上**要发**（= {0,0}，「源头，取它不要钱」）；
 * 不能用 Cost::UNKNOWN 的 {0, 2^62-1}，更不能用 MAX/DRAIN（那是「正在下线」）。 */
int moq_wire_announce_start(MoqBuf *out, const char *suffix,
                            uint64_t warm, uint64_t cold);

/* SUBSCRIBE_START / SUBSCRIBE_END —— subscribe 流上发布端的应答，typed(0x0/0x1)。
 *   [type][size][group(i)]
 * ⚠️ **Lite06 上没有 SUBSCRIBE_OK**：接受是**隐式的**，第一条应答就是 START
 * （lite/subscribe.rs:490-518）。拒绝订阅是**直接 reset 那条流**，不是发一条
 * 消息。START 里的 group 应当是**诚实的** —— 所以要等第一个组真要写出去时才发。 */
int moq_wire_subscribe_start(MoqBuf *out, uint64_t group);
int moq_wire_subscribe_end(MoqBuf *out, uint64_t group);

/* TRACK_INFO —— Track 流（bidi 0x6）上发布端的**唯一**一条应答，写完就 FIN。
 *   [size][priority u8][max_age(i) 毫秒][timescale(i)]
 *
 * ⚠️ **没有 group-order 字节**。那个字段只有 Lite01–05 有，Lite06/07 返回
 * false（rs/moq-net/src/lite/version.rs:164-169）—— 参考实现自带的跨语言断言
 * [06 00 00 53 88 43 E8] 是 **Lite05** 的，照它抄会多一个字节。
 * ⚠️ priority 是 **u8 而不是 varint**。 */
int moq_wire_track_info(MoqBuf *out, uint8_t priority,
                        uint64_t max_age_ms, uint64_t timescale);

/* GROUP 头 —— uni 流上的第一条消息。
 *   [size][subscribe id(i)][group sequence(i)][frame start(i)]
 *
 * ⚠️ 调用方要**先**把 STREAM_TYPE(=0x0，uni 的 Group) 写进同一个缓冲再调这里。
 * ⚠️ 头里的 subscribe id 必须是**对端在 SUBSCRIBE 里选的那个号**，不是那条
 * bidi 流的 stream id —— 报错了对端会把整条组流 STOP_SENDING 掉。
 * ⚠️ frame start 是 06 新增的第三个 varint，**必须写**（我们恒发 0）。少一个
 * varint 后面第一个 FRAME 的时间戳差就整体错位，而且不报错。 */
int moq_wire_group_header(MoqBuf *out, uint64_t sub_id, uint64_t seq,
                          uint64_t frame_start);

/* FRAME —— 裸消息，一串背靠背。
 *   [Timestamp Delta(i, zigzag)][Message Length(i)][Payload]
 * 差值是**组内累加**的相对值（每组第一帧相对 0 起差，基线每组重置）。
 * Message Length **只覆盖 Payload**（不含前面那个差）。 */
int moq_wire_frame(MoqBuf *out, int64_t ts_delta, const void *payload,
                   size_t len);

/* ── 发送 Stream 时一次性用的小助手 ─────────────
 * 控制流上「开流 + 写 STREAM_TYPE + 写第一条消息」是一次 QUIC 发送，
 * 所以需要能把 STREAM_TYPE 也拼进同一个缓冲。 */
int moq_put_stream_type(MoqBuf *out, uint64_t stream_type);

#ifdef __cplusplus
}
#endif

#endif /* MOQ_LITE_WIRE_H */
