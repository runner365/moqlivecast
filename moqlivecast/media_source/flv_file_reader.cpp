#include "flv_file_reader.hpp"

#include "media_packet.hpp"

#include "logger.h"

#include <cerrno>
#include <cstring>

namespace cpp_streamer {

namespace {
constexpr size_t kReadChunk = 256 * 1024;
constexpr size_t kVideoHeaderLen = 5;
constexpr size_t kAudioHeaderLen = 2;
} /* namespace */

struct FlvFileReader::Sink : public CppStreamerInterface {
    FlvFileReader* owner = nullptr;

    std::string StreamerName() override { return "moqlivecast-flv-file-sink"; }
    void SetLogger(Logger* logger) override { logger_ = logger; }
    int AddSinker(CppStreamerInterface*) override { return 0; }
    int RemoveSinker(const std::string&) override { return 0; }
    void StartNetwork(const std::string&, void*) override {}
    void AddOption(const std::string&, const std::string&) override {}
    void SetReporter(StreamerReport* reporter) override { report_ = reporter; }

    int SourceData(Media_Packet_Ptr pkt) override {
        if (!pkt || !owner) return 0;

        const size_t total = pkt->buffer_ptr_->DataLen();
        const uint8_t* base =
            reinterpret_cast<const uint8_t*>(pkt->buffer_ptr_->Data());

        FlvFileFrame f;
        f.dts = pkt->dts_;
        int64_t cts = pkt->pts_ - pkt->dts_;
        if (cts < 0) cts = 0; /* FLV 不应该有负 CTS；异常时钳制并继续 */
        f.cts = cts;
        f.is_seq_hdr = pkt->is_seq_hdr_;
        f.key = pkt->is_key_frame_;

        size_t off = 0;
        if (pkt->av_type_ == MEDIA_VIDEO_TYPE) {
            if (pkt->codec_type_ != MEDIA_CODEC_H264 &&
                pkt->codec_type_ != MEDIA_CODEC_H265) {
                owner->dropped_++;
                if (pkt->codec_type_ != MEDIA_CODEC_UNKNOWN) {
                    if (owner->dropped_ <= 3) {
                        LOG_WARN("[media-source] %s: drop unsupported video codec %d",
                                 owner->path_.c_str(),
                                 static_cast<int>(pkt->codec_type_));
                    }
                } else {
                    LOG_DEBUG("[media-source] %s: ignore video metadata/unknown codec",
                              owner->path_.c_str());
                }
                return 0;
            }
            f.is_video = true;
            f.is_h265 = (pkt->codec_type_ == MEDIA_CODEC_H265);
            off = pkt->flv_offset_;
            if (off == 0) off = kVideoHeaderLen;
        } else if (pkt->av_type_ == MEDIA_AUDIO_TYPE) {
            if (pkt->codec_type_ != MEDIA_CODEC_AAC) {
                owner->dropped_++;
                if (owner->dropped_ <= 3) {
                    LOG_WARN("[media-source] %s: drop unsupported audio codec %d",
                             owner->path_.c_str(), static_cast<int>(pkt->codec_type_));
                }
                return 0;
            }
            off = kAudioHeaderLen;
        } else {
            return 0;
        }

        if (off >= total) {
            LOG_WARN("[media-source] %s: invalid FLV payload offset=%zu total=%zu",
                     owner->path_.c_str(), off, total);
            return 0;
        }
        f.payload.assign(base + off, base + total);
        if (f.dts > owner->last_dts_) owner->last_dts_ = f.dts;
        owner->queue_.push_back(std::move(f));
        return 0;
    }
};

FlvFileReader::FlvFileReader(Logger* logger) : logger_(logger) {}

FlvFileReader::~FlvFileReader() {
    Close();
}

int FlvFileReader::Open(const std::string& path) {
    Close();
    path_ = path;
    fp_ = fopen(path.c_str(), "rb");
    if (!fp_) {
        LOG_ERROR("[media-source] cannot open FLV file: %s", path.c_str());
        return -1;
    }
    eof_ = false;
    last_dts_ = 0;
    dropped_ = 0;
    LOG_INFO("[media-source] opened FLV file: %s", path.c_str());
    return 0;
}

void FlvFileReader::Close() {
    if (fp_) {
        fclose(fp_);
        fp_ = nullptr;
    }
    demux_.reset();
    sink_.reset();
    queue_.clear();
    eof_ = true; /* 没有打开文件时 Fill() 直接返回 false */
}

bool FlvFileReader::Fill() {
    if (eof_ || !fp_) return false;

    if (!demux_) {
        sink_.reset(new Sink());
        sink_->owner = this;
        /* true = 保留 FLV tag body 原文；source 需要裸编码帧 */
        demux_.reset(new FlvDemuxer(true, logger_));
        demux_->AddSinker(sink_.get());
    }

    std::vector<uint8_t> buf(kReadChunk);
    for (;;) {
        const size_t n = fread(buf.data(), 1, buf.size(), fp_);
        if (n > 0) {
            auto pkt = std::make_shared<Media_Packet>(n);
            pkt->buffer_ptr_->AppendData(
                reinterpret_cast<const char*>(buf.data()), n);
            const int ret = demux_->SourceData(pkt);
            if (ret < 0) {
                LOG_ERROR("[media-source] FLV parse failed path=%s ret=%d",
                          path_.c_str(), ret);
                eof_ = true;
                return false;
            }
            if (!queue_.empty()) return true;
        }
        if (n < buf.size()) {
            if (ferror(fp_)) {
                LOG_ERROR("[media-source] FLV read failed path=%s: %s",
                          path_.c_str(), strerror(errno));
            } else {
                LOG_INFO("[media-source] FLV EOF path=%s dts=%lld",
                         path_.c_str(), static_cast<long long>(last_dts_));
            }
            eof_ = true;
            return !queue_.empty();
        }
    }
}

} /* namespace cpp_streamer */
