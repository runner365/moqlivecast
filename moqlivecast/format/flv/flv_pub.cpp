#include "flv_pub.hpp"
#include "utils/byte_stream.hpp"
#include "av/av.hpp"
#include "utils/logger.hpp"
#include "utils/stringex.hpp"

namespace cpp_streamer
{
    MEDIA_CODEC_TYPE GetAudioCodecIdByFlvCodec(uint32_t flv_codec) {
        switch (flv_codec) {
            case FLV_AUDIO_AAC_CODEC:
                return MEDIA_CODEC_AAC;
            case FLV_AUDIO_MP3_CODEC:
                return MEDIA_CODEC_MP3;
            case MAKE_TAG('O', 'p', 'u', 's'):
                return MEDIA_CODEC_OPUS;
            default:
                return MEDIA_CODEC_UNKNOWN;
        }
    }
    MEDIA_CODEC_TYPE GetVideoCodecIdByFlvCodec(uint32_t flv_codec) {
        switch (flv_codec) {
            case FLV_VIDEO_H264_CODEC:
            case MAKE_TAG('a', 'v', 'c', '1'):
                return MEDIA_CODEC_H264;
            case FLV_VIDEO_H265_CODEC:
            case MAKE_TAG('h', 'v', 'c', '1'):
                return MEDIA_CODEC_H265;
            case MAKE_TAG('v', 'p', '0', '8'):
                return MEDIA_CODEC_VP8;
            case MAKE_TAG('v', 'p', '0', '9'):
                return MEDIA_CODEC_VP9;
            case MAKE_TAG('a', 'v', '1', '0'):
                return MEDIA_CODEC_AV1;
            default:
                return MEDIA_CODEC_UNKNOWN;
        }
    }

    int AddFlvMediaHeader(Media_Packet_Ptr pkt_ptr, Logger* logger) {
        uint8_t* p;

        pkt_ptr->fmt_type_ = MEDIA_FORMAT_FLV;
        if (pkt_ptr->av_type_ == MEDIA_AUDIO_TYPE) {
            p = (uint8_t*)pkt_ptr->buffer_ptr_->ConsumeData(-2);

            if (pkt_ptr->codec_type_ == MEDIA_CODEC_AAC) {
                p[0] = FLV_AUDIO_AAC_CODEC | 0x0f;
            } else {
                LogErrorf(logger, "unsuport audio codec type:%d", pkt_ptr->codec_type_);
                return -1;
            }

            if (pkt_ptr->is_seq_hdr_) {
                p[1] = 0x00;
            } else {
                p[1] = 0x01;
            }
        }
        else if (pkt_ptr->av_type_ == MEDIA_VIDEO_TYPE) {
            p = (uint8_t*)pkt_ptr->buffer_ptr_->ConsumeData(-5);

            p[0] = 0;
            if (pkt_ptr->codec_type_ == MEDIA_CODEC_H264) {
                p[0] |= FLV_VIDEO_H264_CODEC;
            } else if (pkt_ptr->codec_type_ == MEDIA_CODEC_H265) {
                p[0] |= FLV_VIDEO_H265_CODEC;
            } else {
                LogErrorf(logger, "unsuport video codec type:%d", pkt_ptr->codec_type_);
                return -1;
            }

            if (pkt_ptr->is_key_frame_ || pkt_ptr->is_seq_hdr_) {
                p[0] |= FLV_VIDEO_KEY_FLAG;
                if (pkt_ptr->is_seq_hdr_) {
                    p[1] = 0x00;
                }
                else {
                    p[1] = 0x01;
                }
            }
            else {
                p[0] |= FLV_VIDEO_INTER_FLAG;
                p[1] = 0x01;
            }
            uint32_t ts_delta = (pkt_ptr->pts_ > pkt_ptr->dts_) ? (uint32_t)(pkt_ptr->pts_ - pkt_ptr->dts_) : 0;
            p[2] = (ts_delta >> 16) & 0xff;
            p[3] = (ts_delta >> 8) & 0xff;
            p[4] = ts_delta & 0xff;
        }

        return 0;
    }

	Media_Packet_Ptr GetFlvMediaPacket(uint8_t type_id, uint32_t ts, const uint8_t* data, int len, Logger* logger) {
        if (len <= 0) {
			return nullptr;
        }
		Media_Packet_Ptr pkt_ptr = std::make_shared<Media_Packet>();

		const uint8_t* p = data;
        uint32_t ts_delta = 0;

        pkt_ptr->typeid_ = type_id;
        pkt_ptr->fmt_type_ = MEDIA_FORMAT_FLV;

        if (type_id == FLV_TAG_VIDEO) {
            uint8_t codec = *p & 0x0f;
            bool enhanced_video_flv = (((*p & 0x80) >> 7) == 1);
            uint8_t video_frame_type = (*p & 0x70) >> 4;
            uint8_t flag = (*p & 0x70) >> 4;
            
            pkt_ptr->av_type_ = MEDIA_VIDEO_TYPE;

            //legacy flv
            if (!enhanced_video_flv) {
                pkt_ptr->flv_offset_ = 5;
                if (codec == FLV_VIDEO_H264_CODEC) {
                    pkt_ptr->codec_type_ = MEDIA_CODEC_H264;
                } else if (codec == FLV_VIDEO_H265_CODEC) {
                    pkt_ptr->codec_type_ = MEDIA_CODEC_H265;
                } else {
                    LogErrorf(logger, "does not support video codec typeid:%d, 0x%02x", type_id, p[0]);
                    //assert(0);
                    return nullptr;
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
                        LogInfof(logger, "input flv video end, ignore it, nalu_type: 0x%02x", nalu_type);
                        return nullptr;
                    }
                    else {
                        LogErrorf(logger, "input flv video error, 0x%02x 0x%02x", p[0], p[1]);
                        return pkt_ptr;
                    }
                } else if (frame_type == FLV_VIDEO_INTER_FLAG) {
                    pkt_ptr->is_key_frame_ = false;
                }
                ts_delta = ByteStream::Read3Bytes(p + 2);
            } else {
                // LogInfof(logger, "input flv video len: %d, flag:0x%02x", len, flag);
                // LogInfoData(logger, (uint8_t*)p, 5, "video packet");
                uint8_t video_packet_type = p[0] & 0x0f;
                p++;
                
                if (video_packet_type == VIDEO_PKTTYPE_MODEX) {
                    // not support to handle VIDEO_PKTTYPE_MODEX
                    LogErrorf(logger, "does not support video packet type: %d", video_packet_type);
                    return nullptr;
                }
                if (video_packet_type != VIDEO_PKTTYPE_META_DATA && video_frame_type == VIDEO_COMMAND_FRAME) {
                    // handle coded frames
                    LogErrorf(logger, "does not support video command frame");
                    return nullptr;
                } else if (video_packet_type == VIDEO_PKTTYPE_MULTITRACK) {
                    uint8_t videoMultitrackType = (*p & 0xf0) >> 4;
                    video_packet_type = *p & 0x0f;
                    p++;

                    if (videoMultitrackType == ManyTracksManyCodecs) {
                        // not support to handle ManyTracksManyCodecs
                        LogErrorf(logger, "does not support video multi track type: %d", videoMultitrackType);
                        return nullptr;
                    }
                    pkt_ptr->codec_type_ = GetVideoCodecIdByFlvCodec(MAKE_TAG(p[0], p[1], p[2], p[3]));
                    p += 4;
                    LogInfof(logger, "video multi track codec type: %s by %c%c%c%c",
                        codectype_tostring(pkt_ptr->codec_type_).c_str(), (char)p[2], (char)p[3], (char)p[4], (char)p[5]);
                } else {
                    pkt_ptr->codec_type_ = GetVideoCodecIdByFlvCodec(MAKE_TAG(p[0], p[1], p[2], p[3]));
                    p += 4;
                    if (video_packet_type == VIDEO_PKTTYPE_META_DATA) {
                        pkt_ptr->av_type_ = MEDIA_METADATA_TYPE;
                        LogInfof(logger, "video single track codec type: %s, video_packet_type:%d",
                            codectype_tostring(pkt_ptr->codec_type_).c_str(), video_packet_type);
                    } else if (video_packet_type == VIDEO_PKTTYPE_SEQUENCE_START) {
                        pkt_ptr->is_seq_hdr_ = true;
                        LogInfof(logger, "video single track codec type: %s, video_packet_type:%d",
                            codectype_tostring(pkt_ptr->codec_type_).c_str(), video_packet_type);
                    } else if (video_packet_type == VIDEO_PKTTYPE_CODEDFRAMES || video_packet_type == VIDEO_PKTTYPE_CODED_FRAMES_X) {
                        if (flag == 1) {
                            pkt_ptr->is_key_frame_ = true;
                            LogInfof(logger, "video single track codec type: %s, coded frame video_packet_type:%d, is_key_frame:%s",
                                codectype_tostring(pkt_ptr->codec_type_).c_str(), video_packet_type, BOOL2STRING(pkt_ptr->is_key_frame_));
                        }
                        LogDebugf(logger, "video single track codec type: %s, coded frame video_packet_type:%d, is_key_frame:%s",
                            codectype_tostring(pkt_ptr->codec_type_).c_str(), video_packet_type, BOOL2STRING(pkt_ptr->is_key_frame_));
                    } else {
                        LogInfof(logger, "video single track codec type: %s, unhandle video_packet_type:%d",
                            codectype_tostring(pkt_ptr->codec_type_).c_str(), (char)p[0], (char)p[1], (char)p[2], (char)p[3], video_packet_type);
                    }
                }
                pkt_ptr->flv_offset_ = p - data;
            }
        } else if (type_id == FLV_TAG_AUDIO) {
            pkt_ptr->av_type_ = MEDIA_AUDIO_TYPE;
            uint8_t frame_type = p[0] & 0xf0;
            bool extern_flv_enable = false;
            bool is_audio_multi_track = false;

            if (frame_type == FLV_AUDIO_AAC_CODEC) {
                pkt_ptr->codec_type_ = MEDIA_CODEC_AAC;
                if (p[1] == 0x00) {
                    pkt_ptr->is_seq_hdr_ = true;
                }
                else if (p[1] == 0x01) {
                    pkt_ptr->is_key_frame_ = false;
                    pkt_ptr->is_seq_hdr_ = false;
                }
                pkt_ptr->flv_offset_ = 2;
            } else if (frame_type == FLV_AUDIO_EX_HEADER) {
                uint8_t audio_packet_type = *p & 0x0f;
                extern_flv_enable = true;
                p++;
                
                if (audio_packet_type == AUDIO_PKTTYPE_MODEX) {
                    // not support to handle AUDIO_PKTTYPE_MODEX
                    LogErrorf(logger, "does not support audio packet type: %d", audio_packet_type);
                    return nullptr;
                }
                if (audio_packet_type == (uint8_t)AUDIO_PKTTYPE_MULTI_TRACK) {
                    is_audio_multi_track = true;
                    uint8_t audio_multi_track_type = (*p & 0xf0) >> 4;
                    audio_packet_type = *p & 0x0f;
                    p++;
                    if (audio_multi_track_type == ManyTracksManyCodecs) {
                        // not support to handle ManyTracksManyCodecs
                        LogErrorf(logger, "does not support audio multi track type: %d", audio_multi_track_type);
                        return nullptr;
                    }

                    pkt_ptr->codec_type_ = GetAudioCodecIdByFlvCodec(MAKE_TAG(p[0], p[1], p[2], p[3]));
                    p += 4;
                    LogInfof(logger, "audio multi track codec type: %s, extern_flv_enable:%s, is_audio_multi_track:%s",
                        codectype_tostring(pkt_ptr->codec_type_).c_str(), BOOL2STRING(extern_flv_enable), BOOL2STRING(is_audio_multi_track));
                } else {
                    pkt_ptr->codec_type_ = GetAudioCodecIdByFlvCodec(MAKE_TAG(p[0], p[1], p[2], p[3]));
                    p += 4;
                    if (audio_packet_type == AUDIO_PKTTYPE_SEQUENCE_START) {
                        LogInfof(logger, "audio single track codec type: %s audio packet type:%d, extern_flv_enable:%s",
                            codectype_tostring(pkt_ptr->codec_type_).c_str(), audio_packet_type, BOOL2STRING(extern_flv_enable));
                        pkt_ptr->is_seq_hdr_ = true;
                    } else if (audio_packet_type == AUDIO_PKTTYPE_CODED_FRAMES) {
                        LogDebugf(logger, "audio single track codec type: %s, audio packet type:%d, extern_flv_enable:%s",
                            codectype_tostring(pkt_ptr->codec_type_).c_str(), audio_packet_type, BOOL2STRING(extern_flv_enable));
                    } else if (audio_packet_type == AUDIO_PKTTYPE_MULTI_CHANNEL_CFG) {
                        LogInfof(logger, "audio single track codec type: %s, audio packet type:%d, extern_flv_enable:%s",
                            codectype_tostring(pkt_ptr->codec_type_).c_str(), audio_packet_type, BOOL2STRING(extern_flv_enable));
                        pkt_ptr->av_type_ = MEDIA_METADATA_TYPE;
                    } else {
                        LogErrorf(logger, "does not support audio packet type: %d, single track codec type: %s, extern_flv_enable:%s",
                            audio_packet_type, codectype_tostring(pkt_ptr->codec_type_).c_str(), BOOL2STRING(extern_flv_enable));
                        return nullptr;
                    }
                }
                pkt_ptr->flv_offset_ = p - data;
            } else {
                LogErrorf(logger, "does not support audio codec typeid:%d, 0x%02x, extern_flv_enable:%s", type_id, p[0], BOOL2STRING(extern_flv_enable));
                return nullptr;
            }
        }
        else if (type_id == FLV_TAG_META_DATA3 || type_id == FLV_TAG_META_DATA0 || type_id == FLV_TAG_AMF0) {
            pkt_ptr->av_type_ = MEDIA_METADATA_TYPE;
        }
        else {
            LogWarnf(logger, "rtmp input unkown media type:%d", type_id);
            return nullptr;
        }

        if (ts_delta > 500) {
            LogWarnf(logger, "video ts_delta error:%u", ts_delta);
        }
        pkt_ptr->dts_ = ts;
        pkt_ptr->pts_ = pkt_ptr->dts_ + ts_delta;

        return pkt_ptr;
	}

}