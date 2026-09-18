#ifndef FLV2MOQ_MOQ_WIRE_HPP
#define FLV2MOQ_MOQ_WIRE_HPP

/* ============================================
 * MOQT 线格式编码 (draft-ietf-moq-transport + draft-ietf-moq-loc)
 *
 * 与 moqlivecast 服务端的 moq_push.cpp 解析逻辑一一对应：
 *   SETUP    → ParseControl (kMoqtSetup)
 *   PUBLISH  → ParsePublish
 *   SUBGROUP → ParseSubgroupHeader
 *   OBJECT   → ParseObject
 *
 * 服务端解析器是唯一权威，改动这里务必对照 moq_push.cpp。
 * ============================================ */

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace flv2moq {

/* draft-ietf-moq-transport 控制消息类型 */
constexpr uint64_t kMoqtSetup = 0x2f00;
constexpr uint64_t kMoqtPublish = 0x1d;

/* draft-ietf-moq-loc-04 属性 ID */
constexpr uint64_t kLocTimescale = 0x08;
constexpr uint64_t kLocFrameMark = 0x09;
constexpr uint64_t kLocVideoConfig = 0x0d;
constexpr uint64_t kLocAudioConfig = 0x0f;
constexpr uint64_t kLocTimestamp = 0x10;

/* 私有扩展：CompositionTime Offset（pts - dts），单位同 Timescale。
 *
 * LOC-04 只定义了单个 Timestamp，无法表达 B 帧的「解码顺序 ≠ 显示顺序」。
 * 缺了它，服务端重建 FLV 时只能把 CompositionTime 写 0（pts == dts），
 * 播放端按解码顺序渲染 → 画面前后跳动（ffplay 能从码流自纠，MSE 不能）。
 *
 * 0x0e 取自 LOC-04 尚未占用的 ID，偶数 ID 的取值直接是 vi64、无需长度前缀。
 * 服务端 ParseObject 对未知属性是 continue 跳过，所以新增不破坏兼容。 */
constexpr uint64_t kLocCompositionTime = 0x0e;

/* 轨道 alias：PUBLISH 里声明，SUBGROUP/OBJECT 里引用。
 * 取值必须与服务端 moq_push.cpp 的 kAliasVideo/kAliasAudio 一致。 */
constexpr uint64_t kAliasVideo = 1;
constexpr uint64_t kAliasAudio = 2;

/* SUBGROUP 头 flags：PROPERTIES(bit0) | bit4 | DEFAULT_PRIORITY(0)
 * 服务端 ParseSubgroupHeader 会跳过 1 字节 priority（(flags & 0x20) == 0）
 * 并设置 has_props = (flags & 0x01)，所以 OBJECT 必须带 properties。 */
constexpr uint64_t kSubgroupFlags = 0x31;

using Bytes = std::vector<uint8_t>;

/* ── varint（QUIC 可变长整数，与服务端 MoqReadVarint 对应）── */
void AppendVarint(Bytes &out, uint64_t v);
void AppendU16(Bytes &out, uint16_t v);
void AppendBytes(Bytes &out, const uint8_t *p, size_t n);
void AppendBytes(Bytes &out, const Bytes &b);

/* ── LOC 属性表 ──
 * even 类型（低 bit = 0）值是 varint 数字；odd 类型值是字节串。
 * 编码时按 type 升序列出，每个条目的 type 用「相对前一个的 delta」表示。 */
struct LocProps {
    uint64_t timestamp_ms = 0;        /* 解码时间戳 DTS */
    /* pts - dts，非负（FLV 封装时已抬高 PTS 保证）。
     * 音频无 B 帧，恒 0；视频按实际值传，服务端写回 FLV tag 的 CompositionTime。 */
    uint64_t cts = 0;
    bool key = false;                 /* 关键帧 → frame_mark = 0x01 */
    const uint8_t *video_cfg = nullptr;  /* avcC，首个视频帧带一次 */
    size_t video_cfg_len = 0;
    const uint8_t *audio_cfg = nullptr;  /* AAC ASC，首个音频帧带一次 */
    size_t audio_cfg_len = 0;
};

/* ── 编码入口 ── */

/* SETUP：0x2f00 | u16(len=0)，发到 control 流 */
Bytes EncodeSetup();

/* PUBLISH：0x1d | u16(len) | body，发到 control 流
 * body = req_id | ns_n(2) | [str app, str stream] | str name | alias | 0 | 0
 * 服务端 ParsePublish 按 [app, stream] 取 namespace，两者都非空才算成功。 */
Bytes EncodePublish(uint64_t request_id, const std::string &app,
                    const std::string &stream, const std::string &track_name,
                    uint64_t alias);

/* SUBGROUP 头：0x31 | alias | group_id，每条第 n 条媒体流的首包 */
Bytes EncodeSubgroupHeader(uint64_t alias, uint64_t group_id);

/* OBJECT：delta | props | payload_len | payload
 * delta 恒为 0 —— 服务端只用 LOC timestamp 定 DTS，不用对象序号。
 * payload 必须是 AVCC(H.264) / 裸 AAC，服务端会原样拼回 FLV tag body。 */
Bytes EncodeObject(const LocProps &props, const uint8_t *payload,
                   size_t payload_len);

} /* namespace flv2moq */

#endif /* FLV2MOQ_MOQ_WIRE_HPP */
