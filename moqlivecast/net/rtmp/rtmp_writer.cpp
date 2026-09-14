#include "rtmp_writer.hpp"
#include "chunk_stream.hpp"
#include "rtmp_session.hpp"
#include "flv_pub.hpp"
#include "logger.hpp"
#include <stdio.h>

namespace cpp_streamer
{
    RtmpWriter::RtmpWriter(RtmpSession* session, Logger* logger) : session_(session)
        , logger_(logger)
    {
        key_ = session->req_.key_;
        LogInfof(logger, "RtmpWriter construct, key:%s", key_.c_str());
    }

    RtmpWriter::~RtmpWriter()
    {
        LogInfof(logger_, "RtmpWriter destruct, key:%s", key_.c_str());
    }
    int RtmpWriter::WritePacket(Media_Packet_Ptr pkt_ptr) {
        uint16_t csid;
        uint8_t  type_id;

        if (closed_) {
            return RTMP_OK;
        }
        if (pkt_ptr->av_type_ == MEDIA_VIDEO_TYPE) {
            csid = 6;
            type_id = RTMP_MEDIA_PACKET_VIDEO;
        }
        else if (pkt_ptr->av_type_ == MEDIA_AUDIO_TYPE) {
            csid = 4;
            type_id = RTMP_MEDIA_PACKET_AUDIO;
        }
        else if (pkt_ptr->av_type_ == MEDIA_METADATA_TYPE) {
            csid = 6;
            type_id = pkt_ptr->typeid_;
        }
        else {
            LogErrorf(logger_, "doesn't support av type:%d, key:%s", (int)pkt_ptr->av_type_, pkt_ptr->key_.c_str());
            return -1;
        }
        int ret = WriteDataByChunkStream(session_, csid,
            (uint32_t)pkt_ptr->dts_, type_id,
            pkt_ptr->streamid_, session_->GetChunkSize(),
            pkt_ptr->buffer_ptr_);
        return ret;
    }

    std::string RtmpWriter::GetKey() {
        return session_->req_.key_;
    }

    std::string RtmpWriter::GetWriterId() {
        return session_->GetSessonKey();
    }

    void RtmpWriter::CloseWriter() {
        LogInfof(logger_, "rtmp writer closed, key:%s", key_.c_str());
        closed_ = true;
    }

    bool RtmpWriter::IsInited() {
        return init_flag_;
    }

    void RtmpWriter::SetInitFlag(bool flag) {
        init_flag_ = flag;
        if (flag) {
            LogInfof(logger_, "it's init packet, so send a gop to client, key:%s", key_.c_str());
        }
    }
}
