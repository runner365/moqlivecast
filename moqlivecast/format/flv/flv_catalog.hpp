#ifndef FLV_CATALOG_HPP
#define FLV_CATALOG_HPP

#include <cstddef>
#include <cstdint>
#include <string>

namespace cpp_streamer {

/* 从 FLV 序列头合成 moq-lite 的 catalog.json。
 *
 * RTMP（以及任何 FLV 风格来源）没有 catalog，但推流开始时一定会发
 * AVC/AAC sequence header —— 那就是合成 catalog 所需的全部信息：
 *   视频：AVCDecoderConfigurationRecord（avcC）或 HEVCDecoderConfigurationRecord（hvcC）
 *   音频：AudioSpecificConfig（ASC）
 *
 * H.265 使用 enhanced-RTMP 的 hvc1 FourCC；catalog 的 codec 字符串为
 * hvc1.*，description 为 hvcC 原字节。
 *
 * 输入是**剥掉 FLV tag 头之后**的载荷，不是完整 tag body。 */
class FlvCatalogBuilder {
public:
    bool OnVideoAvcC(const uint8_t* avcc, size_t len);
    bool OnVideoHvcC(const uint8_t* hvcc, size_t len);
    bool OnAudioAsc(const uint8_t* asc, size_t len);

    bool Empty() const { return !have_video_ && !have_audio_; }
    bool HasVideo() const { return have_video_; }
    bool HasAudio() const { return have_audio_; }

    /* 一条轨都没有时返回空串。 */
    std::string BuildJson() const;

    const std::string& VideoTrack() const { return video_track_; }
    const std::string& AudioTrack() const { return audio_track_; }
    int VideoWidth() const { return video_width_; }
    int VideoHeight() const { return video_height_; }
    int AudioSampleRate() const { return audio_sample_rate_; }
    int AudioChannels() const { return audio_channels_; }

private:
    bool have_video_ = false;
    bool have_audio_ = false;

    /* 轨名沿用本仓约定：catalog 的 rendition key 就是订阅时用的 track 名。 */
    std::string video_track_ = "0.h264";
    std::string audio_track_ = "1.aac";

    std::string video_codec_;      /* avc1.PPCCLL / hvc1.* */
    std::string video_desc_hex_;   /* avcC 原字节的 hex（catalog description） */
    int video_width_ = 0;
    int video_height_ = 0;

    std::string audio_codec_;      /* mp4a.40.X */
    std::string audio_desc_hex_;   /* ASC 原字节的 hex */
    int audio_sample_rate_ = 0;
    int audio_channels_ = 0;
};

} /* namespace cpp_streamer */

#endif /* FLV_CATALOG_HPP */
