#include "rtmp_session_base.hpp"
#include "rtmp_pub.hpp"
#include "flv_pub.hpp"
#include "format/h264_h265_header.hpp"
#include <map>

namespace cpp_streamer
{

const char* server_phase_desc_list[] = {"handshake c2 phase",
                                        "connect phase",
                                        "create stream phase",
                                        "create publish/play phase",
                                        "media handle phase"};

const char* client_phase_desc_list[] = {
    "c0c1 phase", // 0
    "s0s1s2 phase", // 1
    "connect phase", // 2
    "connect response phase",// 3
    "create stream phase",// 4
    "create stream response phase", // 5
    "create play phase", // 6
    "create publish phase", // 7
    "create play publish response phase" // 8
    "media handle phase" // 9
    };

const char* GetServerPhaseDesc(RTMP_SERVER_SESSION_PHASE phase) {
    return server_phase_desc_list[phase];
}

const char* GetClientPhaseDesc(RTMP_CLIENT_SESSION_PHASE phase) {
    return client_phase_desc_list[phase];
}

RtmpSessionBase::RtmpSessionBase(Logger* logger):recv_buffer_(50*1024)
                                                 , logger_(logger)
{
}

RtmpSessionBase::~RtmpSessionBase()
{
}

int RtmpSessionBase::ReadFmtCsid() {
    uint8_t* p = nullptr;

    if (recv_buffer_.Require(1)) {
        p = (uint8_t*)recv_buffer_.Data();
        fmt_  = ((*p) >> 6) & 0x3;
        csid_ = (*p) & 0x3f;
        recv_buffer_.ConsumeData(1);
    } else {
        return RTMP_NEED_READ_MORE;
    }

    if (csid_ == 0) {
        if (recv_buffer_.Require(1)) {//need 1 byte
            p = (uint8_t*)recv_buffer_.Data();
            recv_buffer_.ConsumeData(1);
            csid_ = 64 + *p;
        } else {
            return RTMP_NEED_READ_MORE;
        }
    } else if (csid_ == 1) {
        if (recv_buffer_.Require(2)) {//need 2 bytes
            p = (uint8_t*)recv_buffer_.Data();
            recv_buffer_.ConsumeData(2);
            csid_ = 64;
            csid_ += *p++;
            csid_ += *p;
        } else {
            return RTMP_NEED_READ_MORE;
        }
    } else {
        //normal csid: 2~64
    }

    return RTMP_OK;
}

void RtmpSessionBase::SetChunkSize(uint32_t chunk_size) {
    chunk_size_ = chunk_size;
}

uint32_t RtmpSessionBase::GetChunkSize() {
    return chunk_size_;
}

bool RtmpSessionBase::IsPublish() {
    return req_.publish_flag_;
}

const char* RtmpSessionBase::IsPublishDesc() {
    return req_.publish_flag_ ? "publish" : "play";
}

NALU_FORMAT_TYPE RtmpSessionBase::GetNaluFormatTypeFromMediaPacket(const uint8_t* data, size_t data_len) {
    NALU_FORMAT_TYPE nalu_fmt = NALU_FORMAT_UNKNOWN;
    if (data_len < 5) {
        return nalu_fmt;
    }
    //check avcc format
    if (data[0] == 0x00 && data[1] == 0x00 && data[2] == 0x00 && data[3] == 0x01) {
        nalu_fmt = NALU_FORMAT_ANNEXB;
    } else if (data[0] == 0x00 && data[1] == 0x00 && data[2] == 0x01) {
        nalu_fmt = NALU_FORMAT_ANNEXB;
    } else {
        //avcc format
        nalu_fmt = NALU_FORMAT_AVCC;
    }

    return nalu_fmt;
}

Media_Packet_Ptr RtmpSessionBase::GetMediaPacket(CHUNK_STREAM_PTR cs_ptr) {
    if (cs_ptr->chunk_data_ptr_->DataLen() < 2) {
        LogErrorf(logger_, "rtmp chunk media size:%lu is too small", cs_ptr->chunk_data_ptr_->DataLen());
        return nullptr;
    }
    uint8_t* p = (uint8_t*)cs_ptr->chunk_data_ptr_->Data();
	auto pkt_ptr = GetFlvMediaPacket(cs_ptr->type_id_, cs_ptr->timestamp32_, p, (int)cs_ptr->chunk_data_ptr_->DataLen(), logger_);

    if (!pkt_ptr) {
		LogErrorf(logger_, "Failed to get flv media packet from rtmp chunk stream, type_id: %d", cs_ptr->type_id_);
		return nullptr;
    }
    pkt_ptr->buffer_ptr_->Reset();
    pkt_ptr->buffer_ptr_->AppendData(cs_ptr->chunk_data_ptr_->Data(), cs_ptr->chunk_data_ptr_->DataLen());

    pkt_ptr->app_ = req_.app_;
    pkt_ptr->streamname_ = req_.stream_name_;
    pkt_ptr->key_ = req_.key_;
    pkt_ptr->streamid_ = cs_ptr->msg_stream_id_;
    pkt_ptr->nalu_fmt_type_ = GetNaluFormatTypeFromMediaPacket(
        (uint8_t*)pkt_ptr->buffer_ptr_->Data() + pkt_ptr->flv_offset_,
        (size_t)pkt_ptr->buffer_ptr_->DataLen() - pkt_ptr->flv_offset_);

    if (pkt_ptr->av_type_ == MEDIA_VIDEO_TYPE) {
        if (pkt_ptr->is_seq_hdr_) {
            if (pkt_ptr->codec_type_ == MEDIA_CODEC_H265) {
                HEVC_DEC_CONF_RECORD hevc_dec_info;

                LogInfoData(logger_, (uint8_t*)pkt_ptr->buffer_ptr_->Data() + pkt_ptr->flv_offset_, (size_t)pkt_ptr->buffer_ptr_->DataLen() - pkt_ptr->flv_offset_, "h265 seqhdr data");
                int ret = GetHevcDecInfoFromExtradata(&hevc_dec_info, (uint8_t*)pkt_ptr->buffer_ptr_->Data() + pkt_ptr->flv_offset_, (size_t)pkt_ptr->buffer_ptr_->DataLen() - pkt_ptr->flv_offset_);
                if (ret > 0) {
                    std::string hevc_dump = HevcDecInfoDump(&hevc_dec_info);
                    LogInfof(logger_, "HEVC Decoder Configuration Record, offset:%d\n%s", ret, hevc_dump.c_str());
                    LogInfoData(logger_, (uint8_t*)pkt_ptr->buffer_ptr_->Data() + pkt_ptr->flv_offset_ + ret, (size_t)pkt_ptr->buffer_ptr_->DataLen() - pkt_ptr->flv_offset_ - ret, "h265 data");
                } else {
                    LogErrorf(logger_, "Failed to get hevc decoder configuration record from extradata, ret=%d", ret);
                }
            }
            LogInfof(logger_, "%s seqhdr data, offset:%zu, nalu format:%d",
                codectype_tostring(pkt_ptr->codec_type_).c_str(), pkt_ptr->flv_offset_, pkt_ptr->nalu_fmt_type_);
        }
    } else if (pkt_ptr->av_type_ == MEDIA_AUDIO_TYPE) {
        if (pkt_ptr->is_seq_hdr_) {
            char desc[256];
            snprintf(desc, sizeof(desc), "%s seqhdr data, offset:%zu",
                codectype_tostring(pkt_ptr->codec_type_).c_str(), pkt_ptr->flv_offset_);
            LogInfoData(logger_, (uint8_t*)pkt_ptr->buffer_ptr_->Data(),
                (size_t)pkt_ptr->buffer_ptr_->DataLen(), desc);
        }
    } else if (pkt_ptr->av_type_ == MEDIA_METADATA_TYPE) {
        // LogInfoData(logger_, (uint8_t*)pkt_ptr->buffer_ptr_->Data(),
        //     (size_t)pkt_ptr->buffer_ptr_->DataLen(), "metadata data");
    } else {
        LogErrorf(logger_, "does not support av type: %d", pkt_ptr->av_type_);
        return nullptr;
    }

    return pkt_ptr;

}

int RtmpSessionBase::ReadChunkStream(CHUNK_STREAM_PTR& cs_ptr) {
    int ret = -1;

    if (!fmt_ready_) {
        ret = ReadFmtCsid();
        if (ret != 0) {
            return ret;
        }
        fmt_ready_ = true;
    }

    std::map<uint8_t, CHUNK_STREAM_PTR>::iterator iter = cs_map_.find((uint8_t)csid_);
    if (iter == cs_map_.end()) {
        cs_ptr = std::make_shared<CoChunkStream>(this, fmt_, csid_, chunk_size_, logger_);
        cs_map_.insert(std::make_pair((uint8_t)csid_, cs_ptr));
    } else {
        cs_ptr =iter->second;
        cs_ptr->chunk_size_ = chunk_size_;
    }

    ret = cs_ptr->ReadMessageHeader(fmt_, csid_);
    if ((ret < RTMP_OK) || (ret == RTMP_NEED_READ_MORE)) {
        return ret;
    } else {
        ret = cs_ptr->ReadMessagePayload();
        //cs_ptr->DumpHeader();
        if (ret == RTMP_OK) {
            fmt_ready_ = false;
            //cs_ptr->DumpPayload();
            return ret;
        }
    }

    return ret;
}

}
