#include "httpflv_writer.hpp"
#include "flv_pub.hpp"
#include "byte_stream.hpp"

namespace cpp_streamer
{
    HttpFlvWriter::HttpFlvWriter(std::string key, std::string id,
        std::shared_ptr<HttpResponse> resp, Logger* logger, bool has_video, bool has_audio) :resp_(resp)
        , key_(key)
        , writer_id_(id)
        , has_video_(has_video)
        , has_audio_(has_audio)
        , logger_(logger)
    {
        resp_->AddHeader("Access-Control-Allow-Origin", "*");
        resp_->AddHeader("Access-Control-Allow-Headers", "*");
        resp_->AddHeader("Access-Control-Allow-Methods", "GET, OPTIONS");
        resp_->AddHeader("Content-Type", "video/x-flv");
        resp_->AddHeader("Cache-Control", "no-cache");
        resp_->AddHeader("Connection", "keep-alive");
        alive_ms_ = GetNowMilliSec();
    }

    HttpFlvWriter::~HttpFlvWriter()
    {
        CloseWriter();
    }

    int HttpFlvWriter::SendFlvHeader() {
        /*|'F'(8)|'L'(8)|'V'(8)|version(8)|TypeFlagsReserved(5)|TypeFlagsAudio(1)|TypeFlagsReserved(1)|TypeFlagsVideo(1)|DataOffset(32)|PreviousTagSize(32)|*/
        uint8_t flag = 0;

        if (flv_header_ready_) {
            return 0;
        }

        if (!has_audio_ && !has_video_) {
            return -1;
        }
        if (has_video_) {
            flag |= 0x01;
        }
        if (has_audio_) {
            flag |= 0x04;
        }

        uint8_t flv_header[] = { 0x46, 0x4c, 0x56, 0x01, flag, 0x00, 0x00, 0x00, 0x09, 0x00, 0x00, 0x00, 0x00 };

        resp_->Write((char*)flv_header, sizeof(flv_header), true);

        flv_header_ready_ = true;
        return 0;
    }

    int HttpFlvWriter::WritePacket(Media_Packet_Ptr pkt_ptr) {
        int ret = 0;
        uint8_t flv_header[11];
        uint8_t pre_size_data[4];
        uint32_t pre_size = 0;

        ret = SendFlvHeader();
        if (ret != 0) {
            return 0;
        }
        if (!init_flag_) {
            LogInfof(logger_, "httpflv first tag key=%s type=%d dts=%lld size=%zu",
                     key_.c_str(), (int)pkt_ptr->av_type_,
                     (long long)pkt_ptr->dts_, pkt_ptr->buffer_ptr_->DataLen());
        }

        /*|Tagtype(8)|DataSize(24)|Timestamp(24)|TimestampExtended(8)|StreamID(24)|Data(...)|PreviousTagSize(32)|*/
        if (pkt_ptr->av_type_ == MEDIA_VIDEO_TYPE) {
            flv_header[0] = FLV_TAG_VIDEO;
        }
        else if (pkt_ptr->av_type_ == MEDIA_AUDIO_TYPE) {
            flv_header[0] = FLV_TAG_AUDIO;
        }
        else {
            LogErrorf(logger_, "httpflv writer does not suport av type:%d", pkt_ptr->av_type_);
            return 0;
        }
        uint32_t payload_size = (uint32_t)pkt_ptr->buffer_ptr_->DataLen();
        uint32_t timestamp_base = (uint32_t)(pkt_ptr->dts_ & 0xffffff);
        uint8_t timestamp_ext = (uint8_t)((pkt_ptr->dts_ >> 24) & 0xff);

        ByteStream::Write3Bytes(flv_header + 1, payload_size);
        if (timestamp_base >= 0xffffff) {
            ByteStream::Write3Bytes(flv_header + 4, 0xffffff);
        }
        else {
            ByteStream::Write3Bytes(flv_header + 4, timestamp_base);
        }
        flv_header[7] = timestamp_ext;
        //Set StreamID(24) as 0
        flv_header[8] = 0;
        flv_header[9] = 0;
        flv_header[10] = 0;

		ret = resp_->Write((char*)flv_header, sizeof(flv_header), true);
		if (ret < 0) {
			LogErrorf(logger_, "Failed to write FLV header, status: %d", ret);
			return -1;
		}
        ret = resp_->Write(pkt_ptr->buffer_ptr_->Data(), pkt_ptr->buffer_ptr_->DataLen(), true);
        if (ret < 0) {
            LogErrorf(logger_, "Failed to write FLV data, status: %d", ret);
            return -1;
        }

        pre_size = (uint32_t)(sizeof(flv_header) + pkt_ptr->buffer_ptr_->DataLen());
        ByteStream::Write4Bytes(pre_size_data, pre_size);
        ret = resp_->Write((char*)pre_size_data, sizeof(pre_size_data), true);
        if (ret < 0) {
            LogErrorf(logger_, "Failed to write FLV presize, status: %d", ret);
            return -1;
        }
		alive_ms_ = GetNowMilliSec();
        return 0;
    }

    std::string HttpFlvWriter::GetKey() {
        return key_;
    }

    std::string HttpFlvWriter::GetWriterId() {
        return writer_id_;
    }

    void HttpFlvWriter::CloseWriter() {
        if (closed_flag_) {
            return;
        }
        closed_flag_ = true;
        resp_->Close();
    }

    bool HttpFlvWriter::IsInited() {
        return init_flag_;
    }

    void HttpFlvWriter::SetInitFlag(bool flag) {
        init_flag_ = flag;
    }
    bool HttpFlvWriter::IsAlive() {
        /* 还没等到推流：不要 15s 空闲就拆连接 */
        if (!init_flag_ && !closed_flag_) {
            return true;
        }
        int64_t timeout = 30 * 1000;
		if (closed_flag_) {
            timeout = 1 * 1000;
		}
		int64_t now_ms = GetNowMilliSec();
		if (alive_ms_ <= 0) {
			alive_ms_ = now_ms;
			return true;
		}
		if (now_ms - alive_ms_ > timeout) {
			LogInfof(logger_, "HttpFlvWriter %s is not alive, timeout:%ld", writer_id_.c_str(), (now_ms - alive_ms_));
			return false;
		}
		return true;
    }
}