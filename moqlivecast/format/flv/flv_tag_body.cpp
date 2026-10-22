#include "flv_tag_body.hpp"

#include "flv/flv_pub.hpp"

namespace cpp_streamer {

bool SplitFlvVideoBody(const uint8_t *body, size_t body_len, FlvBodyView *out) {
    if (!body || !out || body_len <= 5) return false;

    const uint8_t frame_type = static_cast<uint8_t>(body[0] >> 4);
    const uint8_t codec = static_cast<uint8_t>(body[0] & 0x0f);
    if (codec != FLV_VIDEO_H264_CODEC) return false;

    const uint8_t avc_packet_type = body[1];
    out->seq = (avc_packet_type == FLV_VIDEO_AVC_SEQHDR);
    out->key = !out->seq && (frame_type == (FLV_VIDEO_KEY_FLAG >> 4));
    out->payload = body + 5;
    out->len = body_len - 5;
    return true;
}

bool SplitFlvAudioBody(const uint8_t *body, size_t body_len, FlvBodyView *out) {
    if (!body || !out || body_len <= 2) return false;

    const uint8_t format = static_cast<uint8_t>(body[0] >> 4);
    if (format != (FLV_AUDIO_AAC_CODEC >> 4)) return false;   /* 只认 AAC */

    out->seq = (body[1] == AUDIO_PKTTYPE_SEQUENCE_START);
    out->key = false;
    out->payload = body + 2;
    out->len = body_len - 2;
    return true;
}

Media_Packet_Ptr MakeFlvVideoPacket(int64_t dts_ms, bool seq, bool key,
                                    const uint8_t *data, size_t len) {
    const size_t body_len = 5 + len;
    auto pkt = std::make_shared<Media_Packet>(body_len);
    pkt->av_type_ = MEDIA_VIDEO_TYPE;
    pkt->codec_type_ = MEDIA_CODEC_H264;
    pkt->fmt_type_ = MEDIA_FORMAT_FLV;
    pkt->dts_ = dts_ms;
    pkt->pts_ = dts_ms;          /* legacy 容器不带 CTS */
    pkt->is_seq_hdr_ = seq;
    pkt->is_key_frame_ = !seq && key;

    const uint8_t hdr[5] = {
        static_cast<uint8_t>((seq || key) ? (FLV_VIDEO_KEY_FLAG | FLV_VIDEO_H264_CODEC)
                                          : (FLV_VIDEO_INTER_FLAG | FLV_VIDEO_H264_CODEC)),
        static_cast<uint8_t>(seq ? FLV_VIDEO_AVC_SEQHDR : FLV_VIDEO_AVC_NALU),
        0, 0, 0,
    };
    pkt->buffer_ptr_->AppendData(reinterpret_cast<const char *>(hdr), 5);
    if (len > 0) {
        pkt->buffer_ptr_->AppendData(reinterpret_cast<const char *>(data),
                                     static_cast<int>(len));
    }
    return pkt;
}

Media_Packet_Ptr MakeFlvAudioPacket(int64_t dts_ms, bool seq,
                                    const uint8_t *data, size_t len) {
    const size_t body_len = 2 + len;
    auto pkt = std::make_shared<Media_Packet>(body_len);
    pkt->av_type_ = MEDIA_AUDIO_TYPE;
    pkt->codec_type_ = MEDIA_CODEC_AAC;
    pkt->fmt_type_ = MEDIA_FORMAT_FLV;
    pkt->dts_ = dts_ms;
    pkt->pts_ = dts_ms;
    pkt->is_seq_hdr_ = seq;
    pkt->is_key_frame_ = seq;
    pkt->has_flv_audio_asc_ = true;

    const uint8_t hdr[2] = {
        static_cast<uint8_t>(FLV_AUDIO_AAC_CODEC | 0x0f),   /* 48 kHz stereo */
        static_cast<uint8_t>(seq ? AUDIO_PKTTYPE_SEQUENCE_START
                                 : AUDIO_PKTTYPE_CODED_FRAMES),
    };
    pkt->buffer_ptr_->AppendData(reinterpret_cast<const char *>(hdr), 2);
    if (len > 0) {
        pkt->buffer_ptr_->AppendData(reinterpret_cast<const char *>(data),
                                     static_cast<int>(len));
    }
    return pkt;
}

} /* namespace cpp_streamer */
