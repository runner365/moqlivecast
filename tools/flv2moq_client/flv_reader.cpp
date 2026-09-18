#include "flv_reader.hpp"

#include "flv_demux.hpp"
#include "media_packet.hpp"

extern "C" {
#include "logger.h"
}

#include <cstdio>
#include <cstring>

namespace flv2moq {

namespace {

constexpr size_t kReadChunk = 256 * 1024;

/* 视频 FLV tag body 固定前 5 字节：
 *   [0] frame_type(4bit) | codec_id(4bit)
 *   [1] AVCPacketType
 *   [2..4] CompositionTime (3B big-endian)
 * 音频固定前 2 字节：
 *   [0] sound_format(4bit) | rate | size | type
 *   [1] AACPacketType
 * 与服务端 EmitFlvVideo / EmitFlvAudio 组包时的偏移严格对应。 */
constexpr size_t kVideoHeaderLen = 5;
constexpr size_t kAudioHeaderLen = 2;

} /* namespace */

/* 承接 FlvDemuxer 解出的 Media_Packet。
 * 定义在 .cpp 里：头文件不必暴露 cpp_streamer 的类型。 */
struct FlvReader::Sink : public cpp_streamer::CppStreamerInterface {
    FlvReader *owner = nullptr;

    std::string StreamerName() override { return "flv2moq-sink"; }
    void SetLogger(cpp_streamer::Logger *logger) override { logger_ = logger; }
    int AddSinker(cpp_streamer::CppStreamerInterface *) override { return 0; }
    int RemoveSinker(const std::string &) override { return 0; }
    void StartNetwork(const std::string &, void *) override {}
    void AddOption(const std::string &, const std::string &) override {}
    void SetReporter(cpp_streamer::StreamerReport *r) override { report_ = r; }

    int SourceData(cpp_streamer::Media_Packet_Ptr pkt_ptr) override {
        cpp_streamer::Media_Packet_Ptr pkt = pkt_ptr;
        if (!pkt || !owner) return 0;

        const size_t total = pkt->buffer_ptr_->DataLen();
        const uint8_t *base =
            reinterpret_cast<const uint8_t *>(pkt->buffer_ptr_->Data());

        FlvFrame f;
        f.dts = static_cast<int64_t>(pkt->dts_);
        /* FlvDemuxer 已算出 pts_ = dts_ + CTS（flv_demux.cpp 里读的 tag 第 2-4 字节）。
         * 负值钳到 0 并告警：FLV 里不该出现负 CTS，一旦出现说明来源异常，
         * 硬编码成 24 位无符号会回绕成 ~16 秒的错位，比丢帧更难排查。 */
        const int64_t cts = static_cast<int64_t>(pkt->pts_) -
                            static_cast<int64_t>(pkt->dts_);
        if (cts < 0) {
            owner->neg_cts_++;
            if (owner->neg_cts_ <= 3) {
                LOG_WARN("[flv2moq] 异常：CTS 为负 (dts=%lld pts=%lld cts=%lld)，已钳为 0",
                        static_cast<long long>(pkt->dts_),
                        static_cast<long long>(pkt->pts_),
                        static_cast<long long>(cts));
            }
            f.cts = 0;
        } else {
            f.cts = cts;
        }
        f.is_seq_hdr = pkt->is_seq_hdr_;
        f.key = pkt->is_key_frame_;

        size_t off = 0;
        if (pkt->av_type_ == cpp_streamer::MEDIA_VIDEO_TYPE) {
            /* 服务端 EmitFlvVideo 写死 H.264（codec_id 固定 7）。
             * HEVC 送过去会被解成 H.264 而乱码，这里直接拦下。 */
            if (pkt->codec_type_ != cpp_streamer::MEDIA_CODEC_H264) {
                owner->dropped_codec_++;
                if (!owner->warned_codec_) {
                    owner->warned_codec_ = true;
                    LOG_WARN("[flv2moq] 仅支持 H.264 视频，当前 codec_type=%d，"
                             "后续非 H.264 视频帧将被丢弃",
                            static_cast<int>(pkt->codec_type_));
                }
                return 0;
            }
            f.is_video = true;
            off = static_cast<size_t>(pkt->flv_offset_);
            if (off == 0) off = kVideoHeaderLen;  /* 兜底 */
        } else if (pkt->av_type_ == cpp_streamer::MEDIA_AUDIO_TYPE) {
            if (pkt->codec_type_ != cpp_streamer::MEDIA_CODEC_AAC) {
                owner->dropped_codec_++;
                return 0; /* 服务端 EmitFlvAudio 只认 AAC */
            }
            f.is_video = false;
            off = kAudioHeaderLen;
        } else {
            return 0; /* metadata / 未知，忽略 */
        }

        if (off >= total) return 0;
        f.payload.assign(base + off, base + total);

        if (f.is_video) owner->video_count_++;
        else owner->audio_count_++;
        if (f.dts > owner->last_dts_) owner->last_dts_ = f.dts;

        owner->queue_.push_back(std::move(f));
        return 0;
    }
};

bool CheckFlvMagic(const std::string &path, std::string &err) {
    FILE *fp = fopen(path.c_str(), "rb");
    if (!fp) {
        err = "无法打开文件";
        return false;
    }
    uint8_t head[9];
    size_t n = fread(head, 1, sizeof(head), fp);
    fclose(fp);
    if (n < 9) {
        err = "文件小于 9 字节，不是 FLV";
        return false;
    }
    if (head[0] != 'F' || head[1] != 'L' || head[2] != 'V') {
        char buf[64];
        snprintf(buf, sizeof(buf), "文件头不是 \"FLV\" (读到 %02x %02x %02x)",
                 head[0], head[1], head[2]);
        err = buf;
        return false;
    }
    return true;
}

FlvReader::FlvReader() = default;

FlvReader::~FlvReader() {
    if (fp_) fclose(fp_);
    /* demux_ 先于 sink_ 析构（声明顺序的反序），此处安全 */
}

int FlvReader::Open(const std::string &path) {
    path_ = path;
    fp_ = fopen(path.c_str(), "rb");
    if (!fp_) {
        LOG_ERROR("[flv2moq] 无法打开文件: %s", path.c_str());
        return -1;
    }
    /* 这里刻意不解析 —— 连接建立后才由 publisher 驱动 Fill()。
     * demuxer 也留到那时再建。 */
    return 0;
}

bool FlvReader::Fill() {
    if (eof_ || !fp_) return false;

    if (!demux_) {
        sink_.reset(new Sink());
        sink_->owner = this;
        /* support_flv_media_hdr=true：保留 FLV tag body 原文（AVCC / 含头 AAC）。
         * 传 false 会得到 Annex-B 且被拆成单个 NALU，服务端拼不回原样。 */
        demux_.reset(new cpp_streamer::FlvDemuxer(true, nullptr));
        demux_->AddSinker(sink_.get());
    }

    std::vector<uint8_t> buf(kReadChunk);
    for (;;) {
        size_t n = fread(buf.data(), 1, buf.size(), fp_);
        if (n > 0) {
            auto pkt = std::make_shared<cpp_streamer::Media_Packet>(n);
            pkt->buffer_ptr_->AppendData(
                reinterpret_cast<const char *>(buf.data()), n);
            int ret = demux_->SourceData(pkt);
            if (ret < 0) {
                LOG_ERROR("[flv2moq] FLV 解析失败 (ret=%d)", ret);
                eof_ = true;
                return false;
            }
            if (!queue_.empty()) return true;
            /* 本 chunk 只喂了文件头还没来得及产出帧，继续读 */
        }
        if (n < buf.size()) {
            if (ferror(fp_)) {
                LOG_ERROR("[flv2moq] 读取错误: %s", path_.c_str());
            }
            eof_ = true;
            return !queue_.empty();
        }
    }
}

} /* namespace flv2moq */
