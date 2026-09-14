#include "gop_cache.hpp"

namespace cpp_streamer {

GopCache::GopCache(Logger *logger, uint32_t min_gop)
    : logger_(logger)
    , min_gop_(min_gop == 0 ? 1 : min_gop) {
}

void GopCache::Input(Media_Packet_Ptr pkt) {
    if (!pkt) return;

    if (pkt->is_seq_hdr_) {
        if (pkt->av_type_ == MEDIA_AUDIO_TYPE) {
            audio_hdr_ = pkt->copy();
        } else if (pkt->av_type_ == MEDIA_VIDEO_TYPE) {
            video_hdr_ = pkt->copy();
        }
        return;
    }

    /* 仅视频关键帧开新 GOP；音频 tag 也会标 is_key_frame，不能用来清空 */
    if (pkt->av_type_ == MEDIA_VIDEO_TYPE && pkt->is_key_frame_) {
        packets_.clear();
    }

    packets_.push_back(pkt->copy());
    (void)min_gop_;
}

size_t GopCache::InsertPacket(Media_Packet_Ptr pkt) {
    Input(pkt);
    return packets_.size();
}

void GopCache::Clear() {
    packets_.clear();
    audio_hdr_.reset();
    video_hdr_.reset();
}

int GopCache::WriterGop(AvWriterInterface *writer) {
    if (!writer) return -1;
    int ret = 0;

    if (video_hdr_ && video_hdr_->buffer_ptr_ &&
        video_hdr_->buffer_ptr_->DataLen() > 0) {
        ret = writer->WritePacket(video_hdr_);
        if (ret < 0) return ret;
    }
    if (audio_hdr_ && audio_hdr_->buffer_ptr_ &&
        audio_hdr_->buffer_ptr_->DataLen() > 0) {
        ret = writer->WritePacket(audio_hdr_);
        if (ret < 0) return ret;
    }
    for (auto &pkt : packets_) {
        ret = writer->WritePacket(pkt);
        if (ret < 0) return ret;
    }
    return ret;
}

} /* namespace cpp_streamer */
