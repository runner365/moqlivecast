#include "media_source_sink.hpp"

#include "flv_h265.hpp"
#include "h264_h265_header.hpp"
#include "media_stream_manager.hpp"

#include "logger.h"

#include <cerrno>
#include <cstring>

namespace cpp_streamer {

namespace {

std::string CodecName(MediaCodec codec) {
    switch (codec) {
        case MEDIA_CODEC_H264: return "H264";
        case MEDIA_CODEC_H265: return "H265";
        case MEDIA_CODEC_AAC: return "AAC";
        default: return "unknown";
    }
}

bool AnnexBToAvcc(const uint8_t* data, size_t len, std::vector<uint8_t>* out) {
    if (!data || len == 0 || !out) return false;
    std::vector<std::shared_ptr<DataBuffer>> nalus;
    if (!AnnexB2Avcc(const_cast<uint8_t*>(data), len, nalus)) return false;
    out->clear();
    for (const auto& nalu : nalus) {
        if (!nalu || nalu->DataLen() == 0) continue;
        out->insert(out->end(),
                    reinterpret_cast<const uint8_t*>(nalu->Data()),
                    reinterpret_cast<const uint8_t*>(nalu->Data()) + nalu->DataLen());
    }
    return !out->empty();
}

} /* namespace */

MediaSourceSink::MediaSourceSink(uv_loop_t* loop,
                                 std::string app,
                                 std::string stream,
                                 std::string source_id)
    : loop_(loop),
      app_(std::move(app)),
      stream_(std::move(stream)),
      source_id_(std::move(source_id)),
      stream_key_(app_ + "/" + stream_),
      owner_id_("local-source:" + source_id_) {
    if (loop_) {
        if (uv_async_init(loop_, &async_, &MediaSourceSink::OnAsync) == 0) {
            async_.data = this;
            async_ready_ = true;
        } else {
            LOG_ERROR("[media-source] %s uv_async_init failed",
                      source_id_.c_str());
        }
    } else {
        LOG_ERROR("[media-source] %s null uv loop", source_id_.c_str());
    }
}

MediaSourceSink::~MediaSourceSink() {
    Stop();
    /* 不在这里 uv_close：manager 的 sink 往往在 uv loop 的 signal 回调里
     * 被销毁，而 close 请求要到下一次 loop 迭代才真正完成；此时句柄内存
     * 已经释放，libuv 会在 uv__finish_close 里访问野指针。
     *
     * 第一版 source 是进程级静态配置，进程退出时随 loop 一起回收；如果
     * 以后支持运行中动态删 source，再把 sink 改成由 close callback 延迟释放。 */
}

int MediaSourceSink::Start() {
    if (!async_ready_) {
        LOG_ERROR("[media-source] %s sink not initialized", source_id_.c_str());
        return -1;
    }
    if (started_.exchange(true)) {
        LOG_WARN("[media-source] %s sink already started", source_id_.c_str());
        return 0;
    }

    PublishClaimResult claim = MediaStreamManager::ClaimPublisher(stream_key_, owner_id_);
    if (claim.action == PublishClaim::kRejected) {
        LOG_ERROR("[media-source] %s claim publisher rejected key=%s previous=%s",
                  source_id_.c_str(), stream_key_.c_str(),
                  claim.previous_owner.c_str());
        started_ = false;
        return -1;
    }
    if (claim.action == PublishClaim::kTakeOver) {
        LOG_WARN("[media-source] %s takeover publisher key=%s previous=%s",
                 source_id_.c_str(), stream_key_.c_str(),
                 claim.previous_owner.c_str());
    } else {
        LOG_INFO("[media-source] %s claim publisher key=%s owner=%s action=%d",
                 source_id_.c_str(), stream_key_.c_str(), owner_id_.c_str(),
                 static_cast<int>(claim.action));
    }
    active_ = true;
    return 0;
}

void MediaSourceSink::Stop() {
    const bool was_active = active_.exchange(false);
    started_ = false;
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        queue_.clear();
    }
    if (was_active) {
        MediaStreamManager::ReleasePublisher(stream_key_, owner_id_);
        LOG_INFO("[media-source] %s sink stopped key=%s video=%llu audio=%llu",
                 source_id_.c_str(), stream_key_.c_str(),
                 static_cast<unsigned long long>(video_frames_),
                 static_cast<unsigned long long>(audio_frames_));
    }
}

int MediaSourceSink::Enqueue(Event&& event, bool allow_backpressure) {
    if (!active_.load()) return -ESHUTDOWN;
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        if (allow_backpressure && queue_.size() >= kMaxQueuedFrames) {
            return -EAGAIN;
        }
        queue_.push_back(std::move(event));
    }
    if (uv_async_send(&async_) != 0) {
        LOG_ERROR("[media-source] %s uv_async_send failed",
                  source_id_.c_str());
        return -EIO;
    }
    return 0;
}

int MediaSourceSink::onTrackInfo(const TrackInfo& info) {
    Event event;
    event.type = EventType::TrackInfo;
    event.info = info;
    return Enqueue(std::move(event), false);
}

int MediaSourceSink::onVideo(MediaFrame frame) {
    if (!frame) return -EINVAL;
    Event event;
    event.type = EventType::Video;
    event.frame = std::move(frame);
    return Enqueue(std::move(event), true);
}

int MediaSourceSink::onAudio(MediaFrame frame) {
    if (!frame) return -EINVAL;
    Event event;
    event.type = EventType::Audio;
    event.frame = std::move(frame);
    return Enqueue(std::move(event), true);
}

void MediaSourceSink::onEof() {
    Event event;
    event.type = EventType::Eof;
    Enqueue(std::move(event), false);
}

void MediaSourceSink::onError(int code, const std::string& message) {
    Event event;
    event.type = EventType::Error;
    event.code = code;
    event.message = message;
    Enqueue(std::move(event), false);
}

int MediaSourceSink::onMetadata(const uint8_t*, size_t) {
    LOG_WARN("[media-source] %s metadata is unsupported",
             source_id_.c_str());
    return -ENOTSUP;
}

void MediaSourceSink::OnAsync(uv_async_t* handle) {
    auto* self = static_cast<MediaSourceSink*>(handle->data);
    if (self) self->Drain();
}

void MediaSourceSink::Drain() {
    std::deque<Event> events;
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        events.swap(queue_);
    }

    for (auto& event : events) {
        if (!active_.load() && event.type != EventType::Eof &&
            event.type != EventType::Error) {
            continue;
        }
        switch (event.type) {
            case EventType::TrackInfo:
                HandleTrackInfo(event.info);
                break;
            case EventType::Video:
                HandleVideo(event.frame);
                break;
            case EventType::Audio:
                HandleAudio(event.frame);
                break;
            case EventType::Eof:
                LOG_INFO("[media-source] %s EOF", source_id_.c_str());
                Finish();
                break;
            case EventType::Error:
                LOG_ERROR("[media-source] %s source error code=%d message=%s",
                          source_id_.c_str(), event.code, event.message.c_str());
                Finish();
                break;
        }
    }
}

void MediaSourceSink::HandleTrackInfo(const TrackInfo& info) {
    LOG_INFO("[media-source] %s track info id=%d codec=%s extradata=%zuB",
             source_id_.c_str(), static_cast<int>(info.id),
             CodecName(info.codec).c_str(), info.extradata.size());

    bool updated = false;
    if (info.id == MediaTrackId::Video) {
        if (info.codec == MEDIA_CODEC_H264) {
            updated = catalog_.OnVideoAvcC(info.extradata.data(), info.extradata.size());
        } else if (info.codec == MEDIA_CODEC_H265) {
            updated = catalog_.OnVideoHvcC(info.extradata.data(), info.extradata.size());
        } else {
            LOG_ERROR("[media-source] %s unsupported video codec %d",
                      source_id_.c_str(), static_cast<int>(info.codec));
            return;
        }
    } else {
        if (info.codec != MEDIA_CODEC_AAC) {
            LOG_ERROR("[media-source] %s unsupported audio codec %d",
                      source_id_.c_str(), static_cast<int>(info.codec));
            return;
        }
        updated = catalog_.OnAudioAsc(info.extradata.data(), info.extradata.size());
    }

    if (!updated) {
        LOG_ERROR("[media-source] %s invalid track extradata id=%d codec=%s",
                  source_id_.c_str(), static_cast<int>(info.id),
                  CodecName(info.codec).c_str());
        return;
    }

    /* 除了 catalog，还必须把 sequence header 作为 FLV tag 写进
     * MediaStreamManager。HTTP-FLV / WebSocket-FLV / RTMP 的 writer 依赖
     * GopCache 里的 avcC/hvcC/ASC；只发 catalog 的话，浏览器看到的第一帧
     * 没有解码配置，ffprobe 也会报 "No start code is found"。 */
    WriteSequenceHeader(info);
    PublishCatalog();
}

void MediaSourceSink::WriteSequenceHeader(const TrackInfo& info) {
    if (!info.extradata.empty() && info.extradata.size() > 1024 * 1024) {
        LOG_ERROR("[media-source] %s sequence header too large: %zuB",
                  source_id_.c_str(), info.extradata.size());
        return;
    }

    Media_Packet_Ptr pkt;
    if (info.id == MediaTrackId::Video) {
        if (info.codec == MEDIA_CODEC_H265) {
            std::vector<uint8_t> body = MakeH265ExVideoBody(
                true, true, 0, info.extradata.data(), info.extradata.size());
            pkt = std::make_shared<Media_Packet>(body.size() + 32);
            pkt->buffer_ptr_->AppendData(
                reinterpret_cast<const char*>(body.data()), body.size());
            pkt->flv_offset_ = 5;
        } else if (info.codec == MEDIA_CODEC_H264) {
            pkt = std::make_shared<Media_Packet>(5 + info.extradata.size() + 32);
            const uint8_t hdr[5] = {0x17, 0x00, 0x00, 0x00, 0x00};
            pkt->buffer_ptr_->AppendData(
                reinterpret_cast<const char*>(hdr), sizeof(hdr));
            pkt->buffer_ptr_->AppendData(
                reinterpret_cast<const char*>(info.extradata.data()),
                info.extradata.size());
            pkt->flv_offset_ = 5;
        } else {
            return;
        }
        pkt->av_type_ = MEDIA_VIDEO_TYPE;
        pkt->codec_type_ = info.codec;
        pkt->is_key_frame_ = true;
    } else {
        if (info.codec != MEDIA_CODEC_AAC) return;
        pkt = std::make_shared<Media_Packet>(2 + info.extradata.size() + 32);
        const uint8_t hdr[2] = {0xaf, 0x00};
        pkt->buffer_ptr_->AppendData(
            reinterpret_cast<const char*>(hdr), sizeof(hdr));
        pkt->buffer_ptr_->AppendData(
            reinterpret_cast<const char*>(info.extradata.data()),
            info.extradata.size());
        pkt->flv_offset_ = 2;
        pkt->av_type_ = MEDIA_AUDIO_TYPE;
        pkt->codec_type_ = MEDIA_CODEC_AAC;
        pkt->sample_rate_ = catalog_.AudioSampleRate() > 0
                                ? catalog_.AudioSampleRate() : 48000;
        pkt->channel_ = static_cast<uint8_t>(
            catalog_.AudioChannels() > 0 ? catalog_.AudioChannels() : 2);
    }

    pkt->fmt_type_ = MEDIA_FORMAT_FLV;
    pkt->is_seq_hdr_ = true;
    pkt->dts_ = 0;
    pkt->pts_ = 0;
    pkt->app_ = app_;
    pkt->streamname_ = stream_;
    pkt->key_ = stream_key_;
    const int ret = MediaStreamManager::WriterMediaPacket(pkt);
    if (ret < 0) {
        LOG_ERROR("[media-source] %s sequence header write failed id=%d ret=%d",
                  source_id_.c_str(), static_cast<int>(info.id), ret);
        return;
    }
    LOG_INFO("[media-source] %s sequence header written id=%d codec=%s size=%zu",
             source_id_.c_str(), static_cast<int>(info.id),
             CodecName(info.codec).c_str(), info.extradata.size());
}

void MediaSourceSink::PublishCatalog() {
    const std::string json = catalog_.BuildJson();
    if (json.empty()) return;
    if (json == last_catalog_json_) return;
    if (!MediaStreamManager::SetCatalogJson(stream_key_, owner_id_, json)) {
        LOG_WARN("[media-source] %s catalog update skipped key=%s",
                 source_id_.c_str(), stream_key_.c_str());
        return;
    }
    last_catalog_json_ = json;
    LOG_INFO("[media-source] %s catalog updated key=%s bytes=%zu",
             source_id_.c_str(), stream_key_.c_str(), json.size());
}

void MediaSourceSink::HandleVideo(const MediaFrame& frame) {
    if (!frame) return;
    const size_t raw_len = frame->buffer_ptr_->DataLen();
    if (raw_len == 0) return;

    const uint8_t* raw = reinterpret_cast<const uint8_t*>(frame->buffer_ptr_->Data());
    std::vector<uint8_t> converted;
    const uint8_t* payload = raw;
    size_t payload_len = raw_len;
    if (frame->nalu_fmt_type_ == NALU_FORMAT_ANNEXB) {
        if (!AnnexBToAvcc(raw, raw_len, &converted)) {
            LOG_ERROR("[media-source] %s Annex-B to AVCC failed size=%zu",
                      source_id_.c_str(), raw_len);
            return;
        }
        payload = converted.data();
        payload_len = converted.size();
    }

    int64_t cts = frame->pts_ - frame->dts_;
    if (cts < 0) cts = 0;
    if (cts > 0x7fffff) cts = 0x7fffff;

    Media_Packet_Ptr pkt;
    if (frame->codec_type_ == MEDIA_CODEC_H265) {
        std::vector<uint8_t> body = MakeH265ExVideoBody(
            false, frame->is_key_frame_, cts, payload, payload_len);
        pkt = std::make_shared<Media_Packet>(body.size() + 32);
        pkt->buffer_ptr_->AppendData(
            reinterpret_cast<const char*>(body.data()), body.size());
        pkt->flv_offset_ = 8;
    } else if (frame->codec_type_ == MEDIA_CODEC_H264) {
        const size_t body_len = 5 + payload_len;
        pkt = std::make_shared<Media_Packet>(body_len + 32);
        uint8_t hdr[5] = {
            static_cast<uint8_t>((frame->is_key_frame_ ? 0x17 : 0x27)),
            0x01,
            static_cast<uint8_t>((cts >> 16) & 0xff),
            static_cast<uint8_t>((cts >> 8) & 0xff),
            static_cast<uint8_t>(cts & 0xff),
        };
        pkt->buffer_ptr_->AppendData(reinterpret_cast<const char*>(hdr), 5);
        pkt->buffer_ptr_->AppendData(
            reinterpret_cast<const char*>(payload), payload_len);
        pkt->flv_offset_ = 5;
    } else {
        LOG_ERROR("[media-source] %s unsupported video codec %d",
                  source_id_.c_str(), static_cast<int>(frame->codec_type_));
        return;
    }

    pkt->av_type_ = MEDIA_VIDEO_TYPE;
    pkt->codec_type_ = frame->codec_type_;
    pkt->fmt_type_ = MEDIA_FORMAT_FLV;
    pkt->nalu_fmt_type_ = NALU_FORMAT_AVCC;
    pkt->dts_ = frame->dts_;
    pkt->pts_ = frame->pts_;
    pkt->is_key_frame_ = frame->is_key_frame_;
    pkt->is_seq_hdr_ = false;
    pkt->app_ = app_;
    pkt->streamname_ = stream_;
    pkt->key_ = stream_key_;

    const int ret = MediaStreamManager::WriterMediaPacket(pkt);
    if (ret < 0) {
        LOG_ERROR("[media-source] %s video write failed ret=%d dts=%lld size=%zu",
                  source_id_.c_str(), ret, static_cast<long long>(pkt->dts_),
                  payload_len);
        return;
    }
    ++video_frames_;
    if (!logged_first_video_) {
        logged_first_video_ = true;
        LOG_INFO("[media-source] %s first video dts=%lld pts=%lld key=%d size=%zu",
                 source_id_.c_str(), static_cast<long long>(pkt->dts_),
                 static_cast<long long>(pkt->pts_),
                 pkt->is_key_frame_ ? 1 : 0, payload_len);
    } else if ((video_frames_ % 300) == 0) {
        LOG_INFO("[media-source] %s video frames=%llu dts=%lld",
                 source_id_.c_str(),
                 static_cast<unsigned long long>(video_frames_),
                 static_cast<long long>(pkt->dts_));
    }
}

void MediaSourceSink::HandleAudio(const MediaFrame& frame) {
    if (!frame) return;
    const size_t raw_len = frame->buffer_ptr_->DataLen();
    if (raw_len == 0) return;

    Media_Packet_Ptr pkt = std::make_shared<Media_Packet>(2 + raw_len + 32);
    uint8_t hdr[2] = {0xaf, 0x01};
    pkt->buffer_ptr_->AppendData(reinterpret_cast<const char*>(hdr), 2);
    pkt->buffer_ptr_->AppendData(frame->buffer_ptr_->Data(), raw_len);

    pkt->av_type_ = MEDIA_AUDIO_TYPE;
    pkt->codec_type_ = MEDIA_CODEC_AAC;
    pkt->fmt_type_ = MEDIA_FORMAT_FLV;
    pkt->dts_ = frame->dts_;
    pkt->pts_ = frame->pts_;
    pkt->is_key_frame_ = false;
    pkt->is_seq_hdr_ = false;
    pkt->app_ = app_;
    pkt->streamname_ = stream_;
    pkt->key_ = stream_key_;
    pkt->flv_offset_ = 2;
    pkt->sample_rate_ = catalog_.AudioSampleRate() > 0
                            ? catalog_.AudioSampleRate() : frame->sample_rate_;
    pkt->channel_ = static_cast<uint8_t>(
        catalog_.AudioChannels() > 0 ? catalog_.AudioChannels() : frame->channel_);

    const int ret = MediaStreamManager::WriterMediaPacket(pkt);
    if (ret < 0) {
        LOG_ERROR("[media-source] %s audio write failed ret=%d dts=%lld size=%zu",
                  source_id_.c_str(), ret, static_cast<long long>(pkt->dts_),
                  raw_len);
        return;
    }
    ++audio_frames_;
    if (!logged_first_audio_) {
        logged_first_audio_ = true;
        LOG_INFO("[media-source] %s first audio dts=%lld pts=%lld size=%zu",
                 source_id_.c_str(), static_cast<long long>(pkt->dts_),
                 static_cast<long long>(pkt->pts_), raw_len);
    }
}

void MediaSourceSink::Finish() {
    if (!active_.exchange(false)) return;
    started_ = false;
    MediaStreamManager::ReleasePublisher(stream_key_, owner_id_);
    LOG_INFO("[media-source] %s finish key=%s video=%llu audio=%llu",
             source_id_.c_str(), stream_key_.c_str(),
             static_cast<unsigned long long>(video_frames_),
             static_cast<unsigned long long>(audio_frames_));
}

constexpr size_t MediaSourceSink::kMaxQueuedFrames;

} /* namespace cpp_streamer */
