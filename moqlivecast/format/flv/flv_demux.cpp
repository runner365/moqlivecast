#include "flv_demux.hpp"
#include "logger.hpp"
#include "byte_stream.hpp"
#include "flv_pub.hpp"
#include "uuid.hpp"
#include "media_packet.hpp"
#include "amf0.hpp"
#include "audio_header.hpp"
#include "h264_h265_header.hpp"
#include "utils/stringex.hpp"

#include <stdio.h>

void* make_flvdemux_streamer() {
    cpp_streamer::FlvDemuxer* demuxer = new cpp_streamer::FlvDemuxer(true, nullptr);

    return demuxer;
}

void destroy_flvdemux_streamer(void* streamer) {
    cpp_streamer::FlvDemuxer* demuxer = (cpp_streamer::FlvDemuxer*)streamer;

    delete demuxer;
}

namespace cpp_streamer
{
#define FLV_DEMUX_NAME "flvdemux"

std::map<std::string, std::string> FlvDemuxer::def_options_ = {
    {"re", "false"}
};

FlvDemuxer::FlvDemuxer(bool support_flv_media_hdr, Logger* logger)
    :CppStreamerInterface()
{
    name_ = FLV_DEMUX_NAME;
    name_ += "_";
    name_ += UUID::MakeUUID();
    options_ = def_options_;
    support_flv_media_hdr_ = support_flv_media_hdr;
    logger_ = logger;
}

FlvDemuxer::~FlvDemuxer()
{
}

std::string FlvDemuxer::StreamerName() {
    return name_;
}

int FlvDemuxer::AddSinker(CppStreamerInterface* sinker) {
    if (!sinker) {
        return (int)sinkers_.size();
    }
    sinkers_[sinker->StreamerName()] = sinker;
    return (int)sinkers_.size();
}

int FlvDemuxer::RemoveSinker(const std::string& name) {
    return (int)sinkers_.erase(name);
}

void FlvDemuxer::SetReporter(StreamerReport* reporter) {
    report_ = reporter;
}

int FlvDemuxer::SourceData(Media_Packet_Ptr pkt_ptr) {
    if (!pkt_ptr) {
        return 0;
    }

    return InputPacket(pkt_ptr);
}

void FlvDemuxer::AddOption(const std::string& key, const std::string& value) {
    auto iter = options_.find(key);
    if (iter == options_.end()) {
        std::stringstream ss;
        ss << "the option key:" << key << " does not exist";
        throw CppStreamException(ss.str().c_str());
    }
    options_[key] = value;
    LogInfof(logger_, "set options key:%s, value:%s", key.c_str(), value.c_str());
}

void FlvDemuxer::Report(const std::string& type, const std::string& value) {
    if (report_) {
        report_->OnReport(name_, type, value);
    }
}

int FlvDemuxer::HandleVideoTag(uint8_t* data, int data_len, Media_Packet_Ptr pkt_ptr) {
    uint8_t* p = data;
    uint32_t ts_delta = 0;

    bool enhanced_video_flv = (((*p & 0x80) >> 7) == 1);
    uint8_t video_frame_type = (*p & 0x70) >> 4;
    uint8_t flag = (*p & 0x70) >> 4;
    
    pkt_ptr->av_type_ = MEDIA_VIDEO_TYPE;

    //legacy flv
    if (!enhanced_video_flv) {
        pkt_ptr->flv_offset_ = 5;
        uint8_t codec = p[0] & 0x0f;
        if (codec == FLV_VIDEO_H264_CODEC) {
            pkt_ptr->codec_type_ = MEDIA_CODEC_H264;
        } else if (codec == FLV_VIDEO_H265_CODEC) {
            pkt_ptr->codec_type_ = MEDIA_CODEC_H265;
        } else {
            LogErrorf(logger_, "does not support video codec typeid:%d, 0x%02x", tag_type_, p[0]);
            //assert(0);
            return -1;
        }

        uint8_t frame_type = p[0] & 0xf0;
        uint8_t nalu_type = p[1];
        if (frame_type == FLV_VIDEO_KEY_FLAG) {
            if (nalu_type == FLV_VIDEO_AVC_SEQHDR) {
                pkt_ptr->is_seq_hdr_ = true;
            }
            else if (nalu_type == FLV_VIDEO_AVC_NALU) {
                pkt_ptr->is_key_frame_ = true;
            }
            else if (nalu_type == FLV_VIDEO_AVC_END) {
                LogInfof(logger_, "input flv video end, ignore it, nalu_type: 0x%02x", nalu_type);
                return 0;
            }
            else {
                LogErrorf(logger_, "input flv video error, 0x%02x 0x%02x", p[0], p[1]);
                return 0;
            }
        } else if (frame_type == FLV_VIDEO_INTER_FLAG) {
            pkt_ptr->is_key_frame_ = false;
        }
        ts_delta= ByteStream::Read3Bytes(p + 2);
    } else {
        //enhanced flv
        uint8_t video_packet_type = p[0] & 0x0f;
        p++;

        if (video_packet_type == VIDEO_PKTTYPE_MODEX) {
            LogInfof(logger_, "input enhanced flv video modex packet, do not support it currently, ignore it");
            return -1;
        }
        if (video_packet_type != VIDEO_PKTTYPE_META_DATA && video_frame_type == VIDEO_COMMAND_FRAME) {
            LogInfof(logger_, "input enhanced flv video command frame, ignore it");
            return 0;
        } else if (video_packet_type == VIDEO_PKTTYPE_META_DATA) {
            uint8_t video_multitrack_type = (p[0] & 0xf0) >> 4;
            video_packet_type = p[0] & 0x0f;
            p++;

            if (video_multitrack_type == ManyTracksManyCodecs) {
                LogInfof(logger_, "input enhanced flv video many tracks many codecs, do not support it currently, ignore it");
                return -1;
            }
            pkt_ptr->codec_type_ = GetVideoCodecIdByFlvCodec(MAKE_TAG(p[0], p[1], p[2], p[3]));
            p += 4;
            LogInfof(logger_, "enhanced flv video multitrack type:%d, video codec typeid:%d, tag:0x%04x",
                     video_multitrack_type, pkt_ptr->codec_type_, MAKE_TAG(p[0], p[1], p[2], p[3]));
        } else {
            pkt_ptr->codec_type_ = GetVideoCodecIdByFlvCodec(MAKE_TAG(p[0], p[1], p[2], p[3]));
            p += 4;
            if (video_packet_type == VIDEO_PKTTYPE_META_DATA) {
                pkt_ptr->av_type_ = MEDIA_METADATA_TYPE;
                LogInfof(logger_, "video single track codec type: %s, video_packet_type:%d",
                    codectype_tostring(pkt_ptr->codec_type_).c_str(), video_packet_type);
            } else if (video_packet_type == VIDEO_PKTTYPE_SEQUENCE_START) {
                pkt_ptr->is_seq_hdr_ = true;
                LogInfof(logger_, "video single track codec type: %s, video_packet_type:%d",
                    codectype_tostring(pkt_ptr->codec_type_).c_str(), video_packet_type);
            } else if (video_packet_type == VIDEO_PKTTYPE_CODEDFRAMES || video_packet_type == VIDEO_PKTTYPE_CODED_FRAMES_X) {
                if (flag == 1) {
                    pkt_ptr->is_key_frame_ = true;
                    LogInfof(logger_, "video single track codec type: %s, coded frame video_packet_type:%d, is_key_frame:%s",
                        codectype_tostring(pkt_ptr->codec_type_).c_str(), video_packet_type, BOOL2STRING(pkt_ptr->is_key_frame_));
                }
                LogDebugf(logger_, "video single track codec type: %s, coded frame video_packet_type:%d, is_key_frame:%s",
                    codectype_tostring(pkt_ptr->codec_type_).c_str(), video_packet_type, BOOL2STRING(pkt_ptr->is_key_frame_));
            } else {
                LogInfof(logger_, "video single track codec type: %s, unhandle video_packet_type:%d",
                    codectype_tostring(pkt_ptr->codec_type_).c_str(), (char)p[0], (char)p[1], (char)p[2], (char)p[3], video_packet_type);
            }
        }
        if (pkt_ptr->codec_type_ == MEDIA_CODEC_H264 || pkt_ptr->codec_type_ == MEDIA_CODEC_H265) {
            ts_delta = ByteStream::Read3Bytes(p);
            p += 3;
        } else {
            ts_delta = 0;
        }
        pkt_ptr->flv_offset_ = p - data;
    }
    pkt_ptr->pts_ = pkt_ptr->dts_ + ts_delta;
    if (support_flv_media_hdr_) {
        pkt_ptr->fmt_type_ = MEDIA_FORMAT_FLV;
        pkt_ptr->buffer_ptr_->AppendData((const char*)data, data_len);
    } else {
        pkt_ptr->fmt_type_ = MEDIA_FORMAT_RAW;

        if (pkt_ptr->is_seq_hdr_) {
            if (pkt_ptr->codec_type_ == MEDIA_CODEC_H264 || pkt_ptr->codec_type_ == MEDIA_CODEC_H265) {
                return HandleVideoSeqHdr(data, data_len, pkt_ptr);
            }
        }

        if (pkt_ptr->codec_type_ == MEDIA_CODEC_H264 || pkt_ptr->codec_type_ == MEDIA_CODEC_H265) {
            //convert nalu from avcc to annexb
            uint8_t* p = data + pkt_ptr->flv_offset_;
            size_t nalu_data_len = tag_data_size_ - pkt_ptr->flv_offset_;
            std::vector<std::shared_ptr<DataBuffer>> nalus;
            bool r = Avcc2Nalus(p, nalu_data_len, nalus);

            if (!r || nalus.size() == 0) {
                LogErrorf(logger_, "failed to convert avcc to annexb nalus, data len:%zu", nalu_data_len);
                return -1;
            }
            for (const auto& nalu_data_ptr : nalus) {
                uint8_t* nalu_data = (uint8_t*)nalu_data_ptr->Data();
                size_t nalu_len = nalu_data_ptr->DataLen();

                int pos = GetNaluTypePos(nalu_data);
                if (pos < 3) {
                    LogErrorf(logger_, "nalu type pos error:%d", pos);
                    continue;
                }
                Media_Packet_Ptr nalu_pkt_ptr = std::make_shared<Media_Packet>();
                nalu_pkt_ptr->copy_properties(pkt_ptr);

                if (H264_IS_AUD(nalu_data[pos])) {
                    //skip aud nalu
                    LogDebugf(logger_, "skip aud nalu");
                    continue;
                }
                if (H264_IS_PPS(nalu_data[pos]) || H264_IS_SPS(nalu_data[pos])) {
                    nalu_pkt_ptr->is_seq_hdr_ = true;
                    nalu_pkt_ptr->is_key_frame_ = false;
                }
                if (H264_IS_KEYFRAME(nalu_data[pos])) {
                    nalu_pkt_ptr->is_seq_hdr_ = false;
                    nalu_pkt_ptr->is_key_frame_ = true;
                }
                nalu_pkt_ptr->buffer_ptr_->AppendData((char*)nalu_data, nalu_len);
                SinkData(nalu_pkt_ptr);
            }
            return 0;
        }
        pkt_ptr->buffer_ptr_->AppendData((const char*)(data + pkt_ptr->flv_offset_), data_len - pkt_ptr->flv_offset_);
    }
	SinkData(pkt_ptr);

    return 0;
}

int FlvDemuxer::HandleVideoSeqHdr(uint8_t* data, int data_len, Media_Packet_Ptr pkt_ptr) {

    uint8_t* p = data;
    uint8_t* nalu = p + pkt_ptr->flv_offset_;
    size_t nalu_len = tag_data_size_ - (p - data);
    const uint8_t start_code[4] = {0x00, 0x00, 0x00, 0x01};

    if (pkt_ptr->codec_type_ != MEDIA_CODEC_H264 && pkt_ptr->codec_type_ != MEDIA_CODEC_H265) {
        return 0;
    }
    LogInfof(logger_, "video seq hdr data len:%d, codec:%s", data_len - pkt_ptr->flv_offset_,
             codectype_tostring(pkt_ptr->codec_type_).c_str());
    LogInfoData(logger_, p + pkt_ptr->flv_offset_, data_len - pkt_ptr->flv_offset_, "video seq hdr");
    if (pkt_ptr->codec_type_ == MEDIA_CODEC_H264) {
        uint8_t sps[1024];
        uint8_t pps[1024];
        size_t sps_len = 0;
        size_t pps_len = 0;

        int ret = GetSpsPpsFromExtraData(pps, pps_len, sps, sps_len, nalu, nalu_len);
        if (ret < 0) {
            LogErrorf(logger_, "failed to get sps pps from h264 extradata");
            return -1;
        }
        LogInfoData(logger_, sps, sps_len, "h264 sps");
        LogInfoData(logger_, pps, pps_len, "h264 pps");

        Media_Packet_Ptr sps_ptr = std::make_shared<Media_Packet>();
        Media_Packet_Ptr pps_ptr = std::make_shared<Media_Packet>();

        sps_ptr->copy_properties(pkt_ptr);
        pps_ptr->copy_properties(pkt_ptr);

        sps_ptr->is_seq_hdr_ = true;
        pps_ptr->is_seq_hdr_ = true;

        sps_ptr->is_key_frame_ = false;
        pps_ptr->is_key_frame_ = false;

        sps_ptr->buffer_ptr_->AppendData((char*)start_code, sizeof(start_code));
        pps_ptr->buffer_ptr_->AppendData((char*)start_code, sizeof(start_code));

        sps_ptr->buffer_ptr_->AppendData((char*)sps, sps_len);
        pps_ptr->buffer_ptr_->AppendData((char*)pps, pps_len);

        SinkData(sps_ptr);
        SinkData(pps_ptr);
        return 0;
    } else if (pkt_ptr->codec_type_ == MEDIA_CODEC_H265) {
        HEVC_DEC_CONF_RECORD hevc_dec_info;
        uint8_t sps[1024];
        uint8_t pps[1024];
        uint8_t vps[1024];
        size_t sps_len = 0;
        size_t pps_len = 0;
        size_t vps_len = 0;

        int ret = GetHevcDecInfoFromExtradata(&hevc_dec_info, nalu, nalu_len);
        if (ret < 0) {
            LogErrorf(logger_, "failed to get sps pps from h265 extradata");
            return -1;
        }
        ret = GetVpsSpsPpsFromHevcDecInfo(&hevc_dec_info,
                                        vps, vps_len,
                                        sps, sps_len,
                                        pps, pps_len);
        if (ret < 0) {
            LogErrorf(logger_, "failed to get vps sps pps from hevc dec info");
            return -1;
        }
        Media_Packet_Ptr vps_ptr = std::make_shared<Media_Packet>();
        Media_Packet_Ptr sps_ptr = std::make_shared<Media_Packet>();
        Media_Packet_Ptr pps_ptr = std::make_shared<Media_Packet>();

        vps_ptr->copy_properties(pkt_ptr);
        sps_ptr->copy_properties(pkt_ptr);
        pps_ptr->copy_properties(pkt_ptr);

        sps_ptr->is_seq_hdr_ = true;
        pps_ptr->is_seq_hdr_ = true;
        vps_ptr->is_seq_hdr_ = true;

        vps_ptr->is_key_frame_ = false;
        sps_ptr->is_key_frame_ = false;
        pps_ptr->is_key_frame_ = false;

        sps_ptr->buffer_ptr_->AppendData((char*)start_code, sizeof(start_code));
        pps_ptr->buffer_ptr_->AppendData((char*)start_code, sizeof(start_code));
        vps_ptr->buffer_ptr_->AppendData((char*)start_code, sizeof(start_code));

        sps_ptr->buffer_ptr_->AppendData((char*)sps, sps_len);
        pps_ptr->buffer_ptr_->AppendData((char*)pps, pps_len);
        vps_ptr->buffer_ptr_->AppendData((char*)vps, vps_len);

        SinkData(vps_ptr);
        SinkData(sps_ptr);
        SinkData(pps_ptr);
        return 0;
    } else {
        LogErrorf(logger_, "handle video seq hdr,  does not support video codec typeid:%d", pkt_ptr->codec_type_);
        return -1;
    }
}

int FlvDemuxer::HandleAudioTag(uint8_t* data, int data_len, Media_Packet_Ptr output_pkt_ptr) {
    // data points to the beginning of audio data， such as AAC raw data:
    // 0xaf 0x01 + AAC raw data
    uint8_t* p = data;
    int header_len = 2;

    output_pkt_ptr->av_type_ = MEDIA_AUDIO_TYPE;
    uint8_t codec_id = p[0] & 0xf0;
    bool is_seq_hdr = (p[1] == 0x00);

    if (codec_id == FLV_AUDIO_AAC_CODEC) {
        output_pkt_ptr->codec_type_ = MEDIA_CODEC_AAC;
    } else if (codec_id == FLV_AUDIO_MP3_CODEC) {
        output_pkt_ptr->codec_type_ = MEDIA_CODEC_MP3;
    } else {
        char error_sz[128];
        snprintf(error_sz, sizeof(error_sz), "does not suport audio codec type:0x%02x", p[0]);
        LogErrorf(logger_, error_sz);
        Report("error", error_sz);
        return -1;
    }
    if (is_seq_hdr) {
        if (codec_id == FLV_AUDIO_AAC_CODEC) {
			size_t audio_cfg_len = tag_data_size_ - 2;
            bool ret = GetAudioInfoByAsc(p + 2, audio_cfg_len,
                                   output_pkt_ptr->aac_asc_type_, output_pkt_ptr->sample_rate_,
                                   output_pkt_ptr->channel_);
            if (ret) {
                LogInfof(logger_, "decode asc to get codec type:%d, sample rate:%d, channel:%d",
                        output_pkt_ptr->codec_type_, output_pkt_ptr->sample_rate_,
                        output_pkt_ptr->channel_);
            }
            output_pkt_ptr->has_flv_audio_asc_ = true;
        }
        output_pkt_ptr->is_seq_hdr_ = true;
    } else {
        output_pkt_ptr->is_key_frame_ = true;
        output_pkt_ptr->is_seq_hdr_   = false;
        output_pkt_ptr->aac_asc_type_ = aac_asc_type_;
    }

    if (codec_id == FLV_AUDIO_AAC_CODEC && is_seq_hdr) {
        LogInfof(logger_, "asc header len:%d", tag_data_size_ - header_len);
        LogInfoData(logger_, p + 2, tag_data_size_ - 2, "asc header");
        bool ret = GetAudioInfoByAsc(p + 2, tag_data_size_ - 2,
                               output_pkt_ptr->aac_asc_type_, output_pkt_ptr->sample_rate_,
                               output_pkt_ptr->channel_);
        if (ret) {
            LogInfof(logger_, "decode asc to get codec type:%d, aac codecid:%d, sample rate:%d, channel:%d",
                    output_pkt_ptr->codec_type_, output_pkt_ptr->aac_asc_type_, output_pkt_ptr->sample_rate_,
                    output_pkt_ptr->channel_);
        }
        output_pkt_ptr->has_flv_audio_asc_ = true;
    }
                
    if (support_flv_media_hdr_) {
        output_pkt_ptr->fmt_type_ = MEDIA_FORMAT_FLV;
        output_pkt_ptr->buffer_ptr_->AppendData((const char*)p, data_len);
    } else {
        output_pkt_ptr->fmt_type_ = MEDIA_FORMAT_RAW;
        output_pkt_ptr->buffer_ptr_->AppendData((const char*)(p + header_len), data_len - header_len);
    }
    SinkData(output_pkt_ptr);
    return 0;
}

int FlvDemuxer::HandlePacket() {
    uint8_t* p;
    // uint32_t ts_delta = 0;

    if (!flv_header_ready_) {
        if (!buffer_.Require(FLV_HEADER_LEN + FLV_TAG_PRE_SIZE)) {
            return FLV_RET_NEED_MORE;
        }

        p = (uint8_t*)buffer_.Data();

        if ((p[0] != 'F') || (p[1] != 'L') || (p[2] != 'V')) {
            Report("error", "flv header tag must be \"FLV\"");
            return -1;
        }
        if ((p[4] & 0x01) == 0x01) {
            has_video_ = true;
        }
        if ((p[4] & 0x04) == 0x04) {
            has_audio_ = true;
        }

        if ((p[5] != 0) || (p[6] != 0) || (p[7] != 0) || (p[8] != 9)) {
            Report("error", "flv pretag size error");
            return -1;
        }
        buffer_.ConsumeData(FLV_HEADER_LEN + FLV_TAG_PRE_SIZE);
        LogInfof(logger_, "flv has %s and %s",
                has_video_ ? "video" : "no video",
                has_audio_ ? "audio" : "no audio");
        flv_header_ready_ = true;
    }

    if (!tag_header_ready_) {
        if (!buffer_.Require(FLV_TAG_HEADER_LEN)) {
            return FLV_RET_NEED_MORE;
        }
        p = (uint8_t*)buffer_.Data();
        tag_type_ = p[0];
        p++;
        tag_data_size_ = ByteStream::Read3Bytes(p);
        p += 3;
        tag_timestamp_ = ByteStream::Read3Bytes(p);
        p += 3;
        tag_timestamp_ |= ((uint32_t)p[0]) << 24;

        tag_header_ready_ = true;
        buffer_.ConsumeData(FLV_TAG_HEADER_LEN);
        //LogInfof(logger_, "p[0]:0x%02x.", *(uint8_t*)(buffer_.Data()));
    }

    if (!buffer_.Require(tag_data_size_ + FLV_TAG_PRE_SIZE)) {
        //LogInfof(logger_, "need more data");
        return FLV_RET_NEED_MORE;
    }
    p = (uint8_t*)buffer_.Data();

    Media_Packet_Ptr output_pkt_ptr = std::make_shared<Media_Packet>();

    output_pkt_ptr->fmt_type_ = MEDIA_FORMAT_RAW;
    output_pkt_ptr->key_ = key_;
    output_pkt_ptr->dts_ = tag_timestamp_;
    output_pkt_ptr->pts_ = tag_timestamp_;

    if (tag_type_ == FLV_TAG_AUDIO) {
        int ret = HandleAudioTag(p, tag_data_size_, output_pkt_ptr);
        if (ret < 0) {
            buffer_.ConsumeData(tag_data_size_ + FLV_TAG_PRE_SIZE);
            tag_header_ready_ = false;
            return ret;
        }
    } else if (tag_type_ == FLV_TAG_VIDEO) {
        int ret = HandleVideoTag(p, tag_data_size_, output_pkt_ptr);
        if (ret < 0) {
            buffer_.ConsumeData(tag_data_size_ + FLV_TAG_PRE_SIZE);
            tag_header_ready_ = false;
            return ret;
        }
    } else if (tag_type_ == FLV_TAG_META_DATA0 || tag_type_ == FLV_TAG_META_DATA3) {
        output_pkt_ptr->av_type_ = MEDIA_METADATA_TYPE;
		int ret = DecodeMetaData(p, tag_data_size_, output_pkt_ptr);
        if (ret < 0) {
            buffer_.ConsumeData(tag_data_size_ + FLV_TAG_PRE_SIZE);
            tag_header_ready_ = false;
            Report("error", "decode metadata error");
            return ret;
        }
    } else {
        buffer_.ConsumeData(tag_data_size_ + FLV_TAG_PRE_SIZE);
        tag_header_ready_ = false;
        LogErrorf(logger_, "does not suport tag type:0x%02x", tag_type_);
        return 0;
    }

    buffer_.ConsumeData(tag_data_size_ + FLV_TAG_PRE_SIZE);
    tag_header_ready_ = false;
    return 0;
}

int FlvDemuxer::DecodeMetaData(uint8_t* data, int data_len, Media_Packet_Ptr pkt_ptr) {
    AMF_ITERM item;
    int index = 0;
    bool is_key = true;
    std::stringstream ss;

    ss << "{";
    do {
        if (AMF_Decoder::Decode(data, data_len, item) < 0) {
            LogWarnf(logger_,"metadata decode return data len:%d", data_len);
            return 0;
        }
        if (item.amf_type_ == AMF_DATA_TYPE_STRING
            || item.amf_type_ == AMF_DATA_TYPE_LONG_STRING) {
            if (is_key) {
                is_key = false;
                ss << item.DumpAmf() << ":";
            } else {
                is_key = true;
                ss << item.DumpAmf();
            }
        } else {
            is_key = true;
            ss << item.DumpAmf();
        }
        if (data_len > 0 && is_key) {
            uint8_t next_amf_type = *data;
            if (next_amf_type != (uint8_t)AMF_DATA_TYPE_UNKNOWN
                && next_amf_type != (uint8_t)AMF_DATA_TYPE_OBJECT_END) {
                ss << ",";
            }
        }
        if (index == 0) {
            if (item.GetAmfType() != AMF_DATA_TYPE_STRING) {
                LogErrorf(logger_,"metadata must be string type, the amf type:%d, number:%f", item.GetAmfType(), item.number_);
                return -1;
            }

            if (item.desc_str_ == "onTextData") {
                pkt_ptr->metadata_type_ = METADATA_TYPE_ONTEXTDATA;
            } else if (item.desc_str_ == "onCaption") {
                pkt_ptr->metadata_type_ = METADATA_TYPE_ONCAPTION;
            } else if (item.desc_str_ == "onCaptionInfo") {
                pkt_ptr->metadata_type_ = METADATA_TYPE_ONCAPTIONINFO;
            } else if (item.desc_str_ == "onMetaData") {
                pkt_ptr->metadata_type_ = METADATA_TYPE_ONTMETADATA;
            } else {
                pkt_ptr->metadata_type_ = METADATA_TYPE_UNKNOWN;
                LogErrorf(logger_, "unknown metadata type:%s", item.desc_str_.c_str());
            }
        } else {
            if (item.GetAmfType() == AMF_DATA_TYPE_OBJECT) {
                for (auto& amf_obj : item.amf_obj_) {
                    std::string key = amf_obj.first;
                    if (amf_obj.second->GetAmfType() == AMF_DATA_TYPE_STRING) {
                        pkt_ptr->metadata_[key] = amf_obj.second->desc_str_;
                    } else if (amf_obj.second->GetAmfType() == AMF_DATA_TYPE_NUMBER) {
                        char desc[80];
                        snprintf(desc, sizeof(desc), "%.02f", amf_obj.second->number_);
                        pkt_ptr->metadata_[key] = std::string(desc);
                    } else if (amf_obj.second->GetAmfType() == AMF_DATA_TYPE_BOOL) {
                        pkt_ptr->metadata_[key] = amf_obj.second->enable_ ? "true" : "false";
                    }
                }
            }
        }
        index++;
    } while (data_len > 1);

    ss << "}";

    Report("MetaData", ss.str());
    return 0;
}

int FlvDemuxer::SinkData(Media_Packet_Ptr pkt_ptr) {
    if (options_["re"] == "true") {
        waiter_.Wait(pkt_ptr);
    }
    int ret = 0;
    for (auto& item : sinkers_) {
        ret += item.second->SourceData(pkt_ptr);
    }
    return ret;
}

int FlvDemuxer::InputPacket(Media_Packet_Ptr pkt_ptr) {
    buffer_.AppendData(pkt_ptr->buffer_ptr_->Data(), pkt_ptr->buffer_ptr_->DataLen());
    if (key_.empty() && !pkt_ptr->key_.empty()) {
        key_ = pkt_ptr->key_;
    }
    int ret = 0;
    do {
        ret = HandlePacket();
    } while (ret == 0);
    
    return ret;
}

int FlvDemuxer::InputPacket(const uint8_t* data, size_t data_len, const std::string& key) {
    buffer_.AppendData((char*)data, data_len);
    key_ = key;

    int ret = 0;
    do {
        ret = HandlePacket();
    } while (ret == 0);
    
    return ret;
}

}
