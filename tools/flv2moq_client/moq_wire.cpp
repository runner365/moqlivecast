#include "moq_wire.hpp"

#include <algorithm>

namespace flv2moq {

/* vi64：长度 = 首字节前导 1 的个数 + 1，1~9 字节；9 字节形态首字节 0xFF。
 * 与服务端 moq_varint.hpp 的 MoqAppendVi64 是同一套编码（两份独立实现，
 * 改一处必须同步另一处）。
 *
 * 2026-09-25 起线上用这套。此前这里是 QUIC 变长整数（RFC 9000 §16），
 * 两套【只有 0~63 一致】—— 例如 100 现在是 1 字节 0x64，旧编码是 2 字节
 * 0x40 0x64。换成 vi64 的依据见 docs/moq-transport-gap.md 的 G1。 */
void AppendVi64(Bytes &out, uint64_t v) {
    size_t n = 1;
    while (n <= 8 && v >= (1ull << (7 * n))) n++;
    if (n <= 8) {
        const size_t lead = n - 1;
        const uint8_t mask = (lead == 0)
            ? static_cast<uint8_t>(0x00)
            : static_cast<uint8_t>(0xFFu << (8 - lead));
        out.push_back(static_cast<uint8_t>(
            mask | ((v >> (8 * (n - 1))) & ((1u << (8 - n)) - 1u))));
    } else {
        out.push_back(static_cast<uint8_t>(0xFF));
    }
    for (size_t i = 1; i < n; i++) {
        out.push_back(static_cast<uint8_t>((v >> (8 * (n - 1 - i))) & 0xffu));
    }
}

void AppendU16(Bytes &out, uint16_t v) {
    out.push_back(static_cast<uint8_t>(v >> 8));
    out.push_back(static_cast<uint8_t>(v & 0xff));
}

void AppendBytes(Bytes &out, const uint8_t *p, size_t n) {
    if (!p || n == 0) return;
    out.insert(out.end(), p, p + n);
}

void AppendBytes(Bytes &out, const Bytes &b) {
    AppendBytes(out, b.data(), b.size());
}

namespace {

/* 属性表条目：even 型存数字，odd 型存字节串 */
struct PropItem {
    uint64_t type = 0;
    bool is_odd = false;
    uint64_t num = 0;
    Bytes raw;
};

/* 编码成 KVP body：type 用「相对前一 type 的 delta」，按 type 升序。
 * 与服务端 moq_push.cpp 的 AppendKvp / EncodeProperties 对称。 */
Bytes EncodeProperties(const std::vector<PropItem> &items) {
    std::vector<PropItem> sorted = items;
    std::sort(sorted.begin(), sorted.end(),
              [](const PropItem &a, const PropItem &b) {
                  return a.type < b.type;
              });

    Bytes body;
    uint64_t prev = 0;
    for (const auto &it : sorted) {
        AppendVi64(body, it.type - prev);
        prev = it.type;
        if (it.is_odd) {
            AppendVi64(body, it.raw.size());
            AppendBytes(body, it.raw);
        } else {
            AppendVi64(body, it.num);
        }
    }

    Bytes out;
    AppendVi64(out, body.size());
    AppendBytes(out, body);
    return out;
}

} /* namespace */

Bytes EncodeSetup() {
    Bytes out;
    AppendVi64(out, kMoqtSetup);
    AppendU16(out, 0);
    return out;
}

Bytes EncodePublish(uint64_t request_id, const std::string &app,
                    const std::string &stream, const std::string &track_name,
                    uint64_t alias) {
    Bytes body;
    AppendVi64(body, request_id);
    /* namespace：[app, stream]，服务端要求两者都非空 */
    AppendVi64(body, 2);
    AppendVi64(body, app.size());
    AppendBytes(body, reinterpret_cast<const uint8_t *>(app.data()), app.size());
    AppendVi64(body, stream.size());
    AppendBytes(body, reinterpret_cast<const uint8_t *>(stream.data()),
                stream.size());
    /* track name */
    AppendVi64(body, track_name.size());
    AppendBytes(body, reinterpret_cast<const uint8_t *>(track_name.data()),
                track_name.size());
    AppendVi64(body, alias);
    AppendVi64(body, 0);  /* 无 track 参数 */
    AppendVi64(body, 0);  /* 无订阅参数 */

    Bytes out;
    AppendVi64(out, kMoqtPublish);
    AppendU16(out, static_cast<uint16_t>(body.size()));
    AppendBytes(out, body);
    return out;
}

Bytes EncodeSubgroupHeader(uint64_t alias, uint64_t group_id) {
    Bytes out;
    AppendVi64(out, kSubgroupFlags);
    AppendVi64(out, alias);
    AppendVi64(out, group_id);
    return out;
}

Bytes EncodeObject(const LocProps &props, const uint8_t *payload,
                   size_t payload_len) {
    std::vector<PropItem> items;
    items.push_back({kLocTimescale, false, 1000, {}});
    items.push_back({kLocTimestamp, false, props.timestamp_ms, {}});
    /* 只在非零时携带：绝大多数帧 cts=0，带上会白白增加每帧开销。
     * 服务端读不到该属性时按 0 处理，语义等价。 */
    if (props.cts != 0) {
        items.push_back({kLocCompositionTime, false, props.cts, {}});
    }
    if (props.key) {
        items.push_back({kLocFrameMark, true, 0, Bytes{0x01}});
    }
    if (props.video_cfg && props.video_cfg_len) {
        items.push_back({kLocVideoConfig, true, 0,
                         Bytes(props.video_cfg,
                               props.video_cfg + props.video_cfg_len)});
    }
    if (props.audio_cfg && props.audio_cfg_len) {
        items.push_back({kLocAudioConfig, true, 0,
                         Bytes(props.audio_cfg,
                               props.audio_cfg + props.audio_cfg_len)});
    }

    Bytes out;
    AppendVi64(out, 0);  /* object_id_delta：服务端只用 timestamp */
    Bytes props_enc = EncodeProperties(items);
    AppendBytes(out, props_enc);
    AppendVi64(out, payload_len);
    AppendBytes(out, payload, payload_len);
    return out;
}

} /* namespace flv2moq */
