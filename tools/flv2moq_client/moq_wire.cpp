#include "moq_wire.hpp"

#include <algorithm>

namespace flv2moq {

void AppendVarint(Bytes &out, uint64_t v) {
    if (v < 64) {
        out.push_back(static_cast<uint8_t>(v));
        return;
    }
    if (v < 16384) {
        out.push_back(static_cast<uint8_t>(0x40 | (v >> 8)));
        out.push_back(static_cast<uint8_t>(v & 0xff));
        return;
    }
    if (v < (1ull << 30)) {
        out.push_back(static_cast<uint8_t>(0x80 | ((v >> 24) & 0x3f)));
        out.push_back(static_cast<uint8_t>((v >> 16) & 0xff));
        out.push_back(static_cast<uint8_t>((v >> 8) & 0xff));
        out.push_back(static_cast<uint8_t>(v & 0xff));
        return;
    }
    out.push_back(static_cast<uint8_t>(0xc0 | ((v >> 56) & 0x3f)));
    for (int i = 6; i >= 0; i--) {
        out.push_back(static_cast<uint8_t>((v >> (i * 8)) & 0xff));
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
        AppendVarint(body, it.type - prev);
        prev = it.type;
        if (it.is_odd) {
            AppendVarint(body, it.raw.size());
            AppendBytes(body, it.raw);
        } else {
            AppendVarint(body, it.num);
        }
    }

    Bytes out;
    AppendVarint(out, body.size());
    AppendBytes(out, body);
    return out;
}

} /* namespace */

Bytes EncodeSetup() {
    Bytes out;
    AppendVarint(out, kMoqtSetup);
    AppendU16(out, 0);
    return out;
}

Bytes EncodePublish(uint64_t request_id, const std::string &app,
                    const std::string &stream, const std::string &track_name,
                    uint64_t alias) {
    Bytes body;
    AppendVarint(body, request_id);
    /* namespace：[app, stream]，服务端要求两者都非空 */
    AppendVarint(body, 2);
    AppendVarint(body, app.size());
    AppendBytes(body, reinterpret_cast<const uint8_t *>(app.data()), app.size());
    AppendVarint(body, stream.size());
    AppendBytes(body, reinterpret_cast<const uint8_t *>(stream.data()),
                stream.size());
    /* track name */
    AppendVarint(body, track_name.size());
    AppendBytes(body, reinterpret_cast<const uint8_t *>(track_name.data()),
                track_name.size());
    AppendVarint(body, alias);
    AppendVarint(body, 0);  /* 无 track 参数 */
    AppendVarint(body, 0);  /* 无订阅参数 */

    Bytes out;
    AppendVarint(out, kMoqtPublish);
    AppendU16(out, static_cast<uint16_t>(body.size()));
    AppendBytes(out, body);
    return out;
}

Bytes EncodeSubgroupHeader(uint64_t alias, uint64_t group_id) {
    Bytes out;
    AppendVarint(out, kSubgroupFlags);
    AppendVarint(out, alias);
    AppendVarint(out, group_id);
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
    AppendVarint(out, 0);  /* object_id_delta：服务端只用 timestamp */
    Bytes props_enc = EncodeProperties(items);
    AppendBytes(out, props_enc);
    AppendVarint(out, payload_len);
    AppendBytes(out, payload, payload_len);
    return out;
}

} /* namespace flv2moq */
