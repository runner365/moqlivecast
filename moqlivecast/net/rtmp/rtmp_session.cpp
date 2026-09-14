#include "rtmp_session.hpp"
#include "chunk_stream.hpp"
#include "rtmp_writer.hpp"
#include "net/tcp/tcp_session.hpp"
#include "utils/av/media_stream_manager.hpp"
#include "utils/stringex.hpp"
#include "utils/uuid.hpp"

namespace cpp_streamer
{

RtmpSession::RtmpSession(uv_loop_t* loop,
    uv_stream_t* server_uv_handle,
    Logger* logger)
    : RtmpSessionBase(logger)
    , logger_(logger)
    , hs_(logger)
    , control_handler_(this, logger)
{
    id_ = UUID::MakeUUID2();
	conn_ptr_ = std::make_shared<TcpSession>(loop, server_uv_handle, this, logger);
    MediaStreamManager::SetLogger(logger);
    running_ = true;
    conn_ptr_->AsyncRead();
    LogInfof(logger_, "rtmp sessoin construct id:%s", id_.c_str());
}

RtmpSession::~RtmpSession()
{
    Close();
    LogInfof(logger_, "rtmp sessoin destruct id:%s", id_.c_str());
}

void RtmpSession::OnWrite(int ret_code, size_t sent_size) {
    if (ret_code < 0) {
		LogInfof(logger_, "RTMP session %s write error, ret: %d", id_.c_str(), ret_code);
        Close();
        return;
    }
    conn_ptr_->AsyncRead();
}

void RtmpSession::OnRead(int ret_code, const char* data, size_t data_size) {
    if (ret_code < 0) {
        LogInfof(logger_, "RTMP session %s read error, ret: %d", id_.c_str(), ret_code);
        Close();
        return;
    }
    recv_buffer_.AppendData(data, data_size);

    int ret = HandleRequest();
	if (ret < 0) {
		LogInfof(logger_, "RTMP session %s handle request error, ret: %d", id_.c_str(), ret);
		Close();
		return;
	}
}

bool RtmpSession::IsAlive() {
    int64_t timeout = 15 * 1000;
	if (!running_) {
        timeout = 1000;
	}
    int64_t now_ms = GetNowMilliSec();
    if (alive_ms_ <= 0) {
		alive_ms_ = now_ms;
		return true;
    }
	if (now_ms - alive_ms_ > timeout) {
		LogInfof(logger_, "RTMP session %s is not alive, timeout:%ld", id_.c_str(), now_ms - alive_ms_);
        return false;
	}
    return true;
}

void RtmpSession::Close() {
    if (closed_flag_) {
        return;
    }
    closed_flag_ = true;
    running_ = false;
    LogInfof(logger_, "rtmp session close, request isReady:%s, publish:%s", BOOL2STRING(req_.is_ready_), BOOL2STRING(req_.publish_flag_));
    conn_ptr_->Close();

    if (req_.is_ready_ && !req_.publish_flag_) {
        if (play_writer_) {
            MediaStreamManager::RemovePlayer(play_writer_);
            delete play_writer_;
        }
    }
    else {
        if (!req_.key_.empty()) {
            MediaStreamManager::RemovePublisher(req_.key_);
        }
    }
}

int RtmpSession::HandleRequest() {
    int ret = RTMP_OK;

    alive_ms_ = GetNowMilliSec();

    while(running_ && recv_buffer_.DataLen() > 0) {
        if (phase_ == initial_phase) {
            LogInfof(logger_, "RTMP session is in initial phase, handling C0C1 handshake.");
            ret = hs_.HandleC0C1(recv_buffer_);
            if (ret < 0) {
                LogErrorf(logger_, "handle C0C1 error, ret: %d", ret);
                running_ = false;
                return ret;
            }
            if (ret == RTMP_NEED_READ_MORE) {
                LogInfof(logger_, "need read more data for C0C1");
                return ret;
            }
            if (ret == RTMP_SIMPLE_HANDSHAKE) {
                LogInfof(logger_, "try to rtmp handshake in simple mode");
            }
            recv_buffer_.Reset();

            std::vector<uint8_t> s0s1s2;
            ret = hs_.SendS0S1S2(s0s1s2);
            if (ret < 0) {
                LogErrorf(logger_, "Failed to send S0S1S2, ret: %d", ret);
                running_ = false;
                return ret;
            }
		    LogInfof(logger_, "RTMP session S0S1S2 data size: %zu", s0s1s2.size());

            conn_ptr_->AsyncWrite((char*)s0s1s2.data(), s0s1s2.size());
            phase_ = handshake_c2_phase;
        } else if (phase_ == handshake_c2_phase) {
            LogInfof(logger_, "RTMP session is in handshake C2 phase, handling C2 handshake.");
            ret = hs_.HandleC2(recv_buffer_);
            if (ret < 0) {
                LogErrorf(logger_, "handle C2 error, ret: %d", ret);
                running_ = false;
                return ret;
            }
            if (ret == RTMP_NEED_READ_MORE) {
                LogInfof(logger_, "need read more data for C2");
                return ret;
            }
            phase_ = connect_phase;
            LogInfof(logger_, "RTMP session phase changed to connect_phase, recv buffer len:%zu",
                    recv_buffer_.DataLen());
            if (recv_buffer_.DataLen() == 0) {
                LogInfof(logger_, "RTMP session has no data in buffer");
                return RTMP_NEED_READ_MORE;
            } else {
                LogInfof(logger_, "RTMP session has data in buffer: %zu", recv_buffer_.DataLen());
                continue;
            }
        } else if (phase_ >= connect_phase) {
            ret = ReceiveChunkStream();
            if (ret < 0) {
                running_ = false;
                return ret;
            }

            if (ret == RTMP_OK) {
                ret = RTMP_NEED_READ_MORE;
            }
            return ret;
        }
    }
    return ret;
}

int RtmpSession::ReceiveChunkStream() {
    CHUNK_STREAM_PTR cs_ptr;
    int ret = -1;

    while (true) {
        ret = ReadChunkStream(cs_ptr);
        if ((ret < RTMP_OK) || (ret == RTMP_NEED_READ_MORE)) {
            return ret;
        }
        //check whether chunk stream is ready(data is full)
        if (!cs_ptr || !cs_ptr->IsReady()) {
            if (recv_buffer_.DataLen() > 0) {
                continue;
            }
            return RTMP_NEED_READ_MORE;
        }
        //check whether we need to send rtmp control ack
        //(void)send_rtmp_ack(cs_ptr->chunk_data_ptr_->data_len());
        //Todo: implement RTMP control ack

        if ((cs_ptr->type_id_ >= RTMP_CONTROL_SET_CHUNK_SIZE) && (cs_ptr->type_id_ <= RTMP_CONTROL_SET_PEER_BANDWIDTH)) {
            ret = control_handler_.HandleRtmpControlMessage(cs_ptr, true);
            if (ret < RTMP_OK) {
                cs_ptr->Reset();
                LogErrorf(logger_, "handle rtmp control message error, ret: %d", ret);
                return ret;
            }
            cs_ptr->Reset();
            if (recv_buffer_.DataLen() > 0) {
                continue;
            }
            break;
        } else if (cs_ptr->type_id_ == RTMP_COMMAND_MESSAGES_AMF0) {
            std::vector<AMF_ITERM*> amf_vec;
            ret = control_handler_.HandleClientCommandMessage(cs_ptr, amf_vec);
            if (ret < RTMP_OK) {
                for (auto iter : amf_vec) {
                    AMF_ITERM* temp = iter;
                    delete temp;
                }
                cs_ptr->Reset();
                LogErrorf(logger_, "handle client command message error, ret: %d", ret);
                return ret;
            }
            for (auto iter : amf_vec) {
                AMF_ITERM* temp = iter;
                delete temp;
            }
            cs_ptr->Reset();
            if (req_.is_ready_ && !req_.publish_flag_) {
                // rtmp play is ready
                LogInfof(logger_, "RTMP play is ready, stream name: %s", req_.stream_name_.c_str());
                play_writer_ = new RtmpWriter(this, logger_);
                MediaStreamManager::AddPlayer(play_writer_);
            }
            if (recv_buffer_.DataLen() > 0) {
                continue;
            }
            break;
        } else if (cs_ptr->type_id_ == RTMP_COMMAND_MESSAGES_AMF3) {
            LogErrorf(logger_, "RTMP AMF3 command messages are not supported yet, type_id: %d", cs_ptr->type_id_);
            cs_ptr->Reset();
            return -1;
        } else if ((cs_ptr->type_id_ == RTMP_MEDIA_PACKET_VIDEO) || (cs_ptr->type_id_ == RTMP_MEDIA_PACKET_AUDIO)
                || (cs_ptr->type_id_ == RTMP_COMMAND_MESSAGES_META_DATA0) || (cs_ptr->type_id_ == RTMP_COMMAND_MESSAGES_META_DATA3)) {
            Media_Packet_Ptr pkt_ptr = GetMediaPacket(cs_ptr);
            if (!pkt_ptr) {
                LogErrorf(logger_, "Failed to get media packet from chunk stream, type_id: %d", cs_ptr->type_id_);
                cs_ptr->Reset();
                return -1;
            }
			MediaStreamManager::WriterMediaPacket(pkt_ptr);
            
            cs_ptr->Reset();
            if (recv_buffer_.DataLen() > 0) {
                continue;
            }
        } else {
            LogErrorf(logger_, "Unsupported chunk stream type, type_id: %d", cs_ptr->type_id_);
            cs_ptr->Reset();
            if (recv_buffer_.DataLen() > 0) {
                continue;
            }
        }
    }
    return 0;
}

DataBuffer* RtmpSession::GetRecvBuffer() {
    return &recv_buffer_;
}

int RtmpSession::RtmpSend(char* data, int len) {
    if (!running_) {
        LogInfof(logger_, "RtmpSend error for running is false");
        return -1;
    }
    alive_ms_ = GetNowMilliSec();
	conn_ptr_->AsyncWrite(data, len);
    return len;
}

int RtmpSession::RtmpSend(std::shared_ptr<DataBuffer> data_ptr) {
    if (!running_) {
        LogInfof(logger_, "RtmpSend error for running is false");
        return -1;
    }
    alive_ms_ = GetNowMilliSec();
    conn_ptr_->AsyncWrite(data_ptr);
    return (int)data_ptr->DataLen();
}

std::string RtmpSession::GetSessonKey() {
    if (!conn_ptr_) {
        return "";
    }
    return conn_ptr_->GetRemoteEndpoint();
}

}