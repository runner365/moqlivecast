#include "rtc_sdp_filter.hpp"

namespace cpp_streamer
{
SdpFilter g_sdp_answer_filter;

/*
a=extmap:1 urn:ietf:params:rtp-hdrext:ssrc-audio-level
a=extmap:2 http://www.webrtc.org/experiments/rtp-hdrext/abs-send-time
a=extmap:3 http://www.ietf.org/id/draft-holmer-rmcat-transport-wide-cc-extensions-01
a=extmap:4 urn:ietf:params:rtp-hdrext:sdes:mid
*/
void InitSdpFilter() {
    g_sdp_answer_filter.exts_.push_back("http://www.webrtc.org/experiments/rtp-hdrext/abs-send-time");
    g_sdp_answer_filter.exts_.push_back("http://www.ietf.org/id/draft-holmer-rmcat-transport-wide-cc-extensions-01");
    g_sdp_answer_filter.exts_.push_back("urn:ietf:params:rtp-hdrext:sdes:mid");
    // g_sdp_answer_filter.exts_.push_back("urn:ietf:params:rtp-hdrext:ssrc-audio-level");

    CodecFilter opus_codec_filter;
    opus_codec_filter.media_type_ = MEDIA_AUDIO_TYPE;
    opus_codec_filter.codec_.codec_name_ = "opus";
    //a=fmtp:111 minptime=10;maxaveragebitrate=96000;stereo=1;sprop-stereo=1;useinbandfec=1
    opus_codec_filter.codec_.opus_fmtp_param_ = std::make_unique<OpusCodecFmtpParam>();
    opus_codec_filter.codec_.opus_fmtp_param_->minptime_ = 10;
    opus_codec_filter.codec_.opus_fmtp_param_->useinbandfec_ = 1;
    opus_codec_filter.codec_.opus_fmtp_param_->maxaveragebitrate_ = 96000;
    opus_codec_filter.codec_.opus_fmtp_param_->stereo_ = 1;
    opus_codec_filter.codec_.opus_fmtp_param_->sprop_stereo_ = 1;

    g_sdp_answer_filter.codecs_.push_back(opus_codec_filter);

    CodecFilter h264_codec_filter;
    h264_codec_filter.media_type_ = MEDIA_VIDEO_TYPE;
    h264_codec_filter.codec_.codec_name_ = "H264";
    h264_codec_filter.codec_.h264_fmtp_param_ = std::make_unique<H264CodecFmtpParam>();
    h264_codec_filter.codec_.h264_fmtp_param_->profile_level_id_ = "42e01f";
    h264_codec_filter.codec_.h264_fmtp_param_->packetization_mode_ = 1;
    h264_codec_filter.codec_.h264_fmtp_param_->level_asymmetry_allowed_ = 1;
	g_sdp_answer_filter.codecs_.push_back(h264_codec_filter);
}

} // namespace cpp_streamer