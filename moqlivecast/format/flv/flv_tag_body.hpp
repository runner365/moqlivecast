#ifndef FLV_TAG_BODY_HPP
#define FLV_TAG_BODY_HPP

#include "media_packet.hpp"

#include <cstddef>
#include <cstdint>

namespace cpp_streamer {

/* MediaStreamManager 里的包一律是 **FLV tag body**：
 *   视频 = [FrameType|CodecID][AVCPacketType][CTS×3] + AVCC NALU
 *   音频 = [SoundFormat|...][AACPacketType]           + 裸 AAC
 * 这两个 helper 负责在「FLV 形状」和「裸编解码载荷」之间转换，避免每个
 * 消费方各写一遍偏移量（偏移写错是静默错位，只有对端解码器会报错）。 */

struct FlvBodyView {
    const uint8_t *payload = nullptr;   /* 裸 AVCC / AAC */
    size_t len = 0;
    bool seq = false;                   /* 序列头 */
    bool key = false;                   /* 视频关键帧（音频恒 false） */
};

/* 只支持 legacy FLV（AVC / AAC）。增强 RTMP 的 FourCC 形状返回 false。 */
bool SplitFlvVideoBody(const uint8_t *body, size_t body_len, FlvBodyView *out);
bool SplitFlvAudioBody(const uint8_t *body, size_t body_len, FlvBodyView *out);

/* 由裸载荷拼出 FLV tag body 的 Media_Packet（app/key/stream 由调用方填）。 */
Media_Packet_Ptr MakeFlvVideoPacket(int64_t dts_ms, bool seq, bool key,
                                    const uint8_t *data, size_t len);
Media_Packet_Ptr MakeFlvAudioPacket(int64_t dts_ms, bool seq,
                                    const uint8_t *data, size_t len);

} /* namespace cpp_streamer */

#endif /* FLV_TAG_BODY_HPP */
