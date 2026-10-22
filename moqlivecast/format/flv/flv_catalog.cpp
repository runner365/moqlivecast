#include "flv_catalog.hpp"
#include "h264_h265_header.hpp"

#include <cstdio>

namespace cpp_streamer {

namespace {

/* catalog 的 description 是**小写 hex、无分隔符、无 0x**（与 moq-lite
 * 参考实现和本仓 C 客户端的 pub_build_catalog 一致）。 */
std::string ToHex(const uint8_t* data, size_t len) {
    static const char* kHex = "0123456789abcdef";
    std::string out;
    out.resize(len * 2);
    for (size_t i = 0; i < len; i++) {
        out[2 * i]     = kHex[data[i] >> 4];
        out[2 * i + 1] = kHex[data[i] & 0x0f];
    }
    return out;
}

} /* namespace */

bool FlvCatalogBuilder::OnVideoAvcC(const uint8_t* avcc, size_t len) {
    /* AVCDecoderConfigurationRecord:
     *   [0] configurationVersion
     *   [1] AVCProfileIndication  [2] profile_compatibility  [3] AVCLevelIndication
     *   [4] 0b111111 + lengthSizeMinusOne
     *   [5] 0b111 + numOfSequenceParameterSets
     *   [6..7] spsLength, then SPS NAL (含 1 字节 NAL header) ... */
    if (!avcc || len < 8) return false;

    char codec[32];
    snprintf(codec, sizeof(codec), "avc1.%02x%02x%02x",
             (unsigned)avcc[1], (unsigned)avcc[2], (unsigned)avcc[3]);
    video_codec_ = codec;
    video_desc_hex_ = ToHex(avcc, len);

    int w = 0;
    int h = 0;
    const size_t num_sps = avcc[5] & 0x1f;
    if (num_sps > 0) {
        const size_t sps_len = ((size_t)avcc[6] << 8) | avcc[7];
        if (sps_len > 0 && 8 + sps_len <= len) {
            /* 失败不致命：catalog 里省略宽高，解码器仍按 avcC 工作。 */
            ParseH264SpsDimensions(avcc + 8, (int)sps_len, &w, &h, nullptr);
        }
    }
    video_width_ = w;
    video_height_ = h;
    have_video_ = true;
    return true;
}

bool FlvCatalogBuilder::OnAudioAsc(const uint8_t* asc, size_t len) {
    /* AudioSpecificConfig:
     *   audioObjectType(5) samplingFrequencyIndex(4) channelConfiguration(4) … */
    if (!asc || len < 2) return false;

    const uint8_t object_type = (asc[0] >> 3) & 0x1f;
    if (object_type == 31) return false;   /* escape：本轮不支持 */

    const int freq_idx = ((asc[0] & 0x07) << 1) | (asc[1] >> 7);
    static const int kRates[13] = {
        96000, 88200, 64000, 48000, 44100, 32000,
        24000, 22050, 16000, 12000, 11025, 8000, 7350
    };
    if (freq_idx < 0 || freq_idx >= 13) return false;

    int channels = (asc[1] >> 3) & 0x0f;
    if (channels <= 0) channels = 2;   /* channelConfiguration=0 由 PCE 定义 */

    char codec[32];
    snprintf(codec, sizeof(codec), "mp4a.40.%u", (unsigned)object_type);
    audio_codec_ = codec;
    audio_desc_hex_ = ToHex(asc, len);
    audio_sample_rate_ = kRates[freq_idx];
    audio_channels_ = channels;
    have_audio_ = true;
    return true;
}

std::string FlvCatalogBuilder::BuildJson() const {
    if (Empty()) return std::string();

    std::string j = "{";
    bool first = true;

    if (have_video_) {
        j += "\"video\":{\"renditions\":{\"";
        j += video_track_;
        j += "\":{\"codec\":\"";
        j += video_codec_;
        j += "\",\"description\":\"";
        j += video_desc_hex_;
        j += "\"";
        if (video_width_ > 0 && video_height_ > 0) {
            j += ",\"codedWidth\":" + std::to_string(video_width_);
            j += ",\"codedHeight\":" + std::to_string(video_height_);
        }
        j += "}}}";
        first = false;
    }

    if (have_audio_) {
        if (!first) j += ",";
        j += "\"audio\":{\"renditions\":{\"";
        j += audio_track_;
        j += "\":{\"codec\":\"";
        j += audio_codec_;
        j += "\",\"description\":\"";
        j += audio_desc_hex_;
        j += "\",\"sampleRate\":" + std::to_string(audio_sample_rate_);
        j += ",\"numberOfChannels\":" + std::to_string(audio_channels_);
        j += "}}}";
        first = false;
    }

    /* clock 与 Android 客户端一致：wall=0、timescale=1e6。RTMP 的 dts 是
     * 从 0 开始的毫秒时间轴，内层前缀是绝对微秒。 */
    j += ",\"clock\":{\"wall\":0,\"timescale\":1000000}}";
    return j;
}

} /* namespace cpp_streamer */
