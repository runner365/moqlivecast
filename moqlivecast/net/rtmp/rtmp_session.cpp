#include "rtmp_session.hpp"
#include "rtmp_server.hpp"
#include "chunk_stream.hpp"
#include "rtmp_writer.hpp"
#include "net/tcp/tcp_session.hpp"
#include "utils/av/media_stream_manager.hpp"
#include "utils/stringex.hpp"
#include "utils/uuid.hpp"

namespace cpp_streamer
{

namespace {
/* catalog 合并窗（毫秒）：一个 sequence header 到了之后，先等这么久再发。
 * RTMP 的 video/audio seq header 总是紧挨着到；如果逐条发，订阅方会先看到
 * “只有视频”的中间态 —— moq-mux 的 FLV exporter 在文件头之后不允许已绑定
 * 的轨从 catalog 消失（"FLV track removed mid-stream"），音频会被永久丢掉。
 * 两轨都到齐时立即发；只有一条轨时由下一帧触发 flush。 */
constexpr int64_t kCatalogCoalesceMs = 100;
} /* namespace */


RtmpSession::RtmpSession(uv_loop_t* loop,
    uv_stream_t* server_uv_handle,
    RtmpServer* server,
    int64_t idle_timeout_ms,
    Logger* logger)
    : RtmpSessionBase(logger)
    , logger_(logger)
    , server_(server)
    , hs_(logger)
    , control_handler_(this, logger)
    , idle_timeout_ms_(idle_timeout_ms > 0 ? idle_timeout_ms : 15000)
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
    /* 正常会话：配置的空闲超时（收发都算活动，见 HandleRequest/RtmpSend）。
     * 已关闭/握手失败的会话：1 秒快速回收，别占着 sessions_ 表。 */
    int64_t timeout = running_ ? idle_timeout_ms_ : 1000;
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

void RtmpSession::UpdateCatalogFromSeqHdr(const Media_Packet_Ptr& pkt) {
    if (!pkt || !pkt->buffer_ptr_) return;
    const size_t body_len = pkt->buffer_ptr_->DataLen();
    const size_t off = pkt->flv_offset_;   /* 视频 5 / 音频 2 */
    if (off >= body_len) return;

    const uint8_t* body =
        reinterpret_cast<const uint8_t*>(pkt->buffer_ptr_->Data());
    const uint8_t* payload = body + off;
    const size_t payload_len = body_len - off;

    bool updated = false;
    if (pkt->av_type_ == MEDIA_VIDEO_TYPE) {
        if (pkt->codec_type_ == MEDIA_CODEC_H264) {
            updated = catalog_.OnVideoAvcC(payload, payload_len);
            if (updated) {
                LogInfof(logger_, "RTMP catalog: video avcC %zu B, %dx%d",
                         payload_len, catalog_.VideoWidth(),
                         catalog_.VideoHeight());
            }
        } else {
            LogWarnf(logger_, "RTMP catalog: video codec %d is not H.264 — "
                     "moq-lite export will skip the video track",
                     (int)pkt->codec_type_);
        }
    } else if (pkt->av_type_ == MEDIA_AUDIO_TYPE) {
        if (pkt->codec_type_ == MEDIA_CODEC_AAC) {
            updated = catalog_.OnAudioAsc(payload, payload_len);
            if (updated) {
                LogInfof(logger_, "RTMP catalog: audio ASC %zu B, %d Hz %d ch",
                         payload_len, catalog_.AudioSampleRate(),
                         catalog_.AudioChannels());
            }
        } else {
            LogWarnf(logger_, "RTMP catalog: audio codec %d is not AAC — "
                     "moq-lite export will skip the audio track",
                     (int)pkt->codec_type_);
        }
    }

    if (!updated) return;

    catalog_pending_ = true;
    catalog_pending_ms_ = GetNowMilliSec();
    /* 两轨配置都在 = 这次更新是完整的，立即发；只有一条时等下一帧再发，
     * 避免把“只有视频/只有音频”的中间态推给订阅方。 */
    if (catalog_.HasVideo() && catalog_.HasAudio()) {
        FlushCatalog();
    }
}

void RtmpSession::FlushCatalog() {
    if (!catalog_pending_) return;
    catalog_pending_ = false;
    const std::string json = catalog_.BuildJson();
    if (json.empty()) return;
    if (MediaStreamManager::SetCatalogJson(req_.key_, id_, json)) {
        LogInfof(logger_, "RTMP catalog updated key=\"%s\": %zu bytes",
                 req_.key_.c_str(), json.size());
    }
}

bool RtmpSession::OnPublishReady() {
    if (claimed_) return true;
    if (!req_.publish_flag_ || req_.key_.empty()) return true;

    /* 同一个 app/stream 同时只能有一个 publisher（draft §2.4.3 的 Full Track
     * Name 唯一性）。规则与 moq-lite / MOQ 两条 ingest 完全一致：
     * 新会话顶掉旧会话；被顶掉的旧会话在 owner 校验里停止写帧。 */
    const PublishClaimResult claim =
        MediaStreamManager::ClaimPublisher(req_.key_, id_);
    switch (claim.action) {
    case PublishClaim::kTakeOver:
        LogWarnf(logger_, "RTMP publish take over key=\"%s\": old owner=%s new=%s",
                 req_.key_.c_str(), claim.previous_owner.c_str(), id_.c_str());
        if (!server_ || !server_->KickSession(claim.previous_owner)) {
            /* 旧 owner 不是 RTMP 会话（moq-lite / WebTransport）。它的每一帧
             * 都会在 owner 校验里被丢掉，连接等 idle 超时自然消失。 */
            LogInfof(logger_, "RTMP takeover: old owner %s is not an RTMP session; "
                     "it stops feeding via the ownership check",
                     claim.previous_owner.c_str());
        }
        break;
    case PublishClaim::kRejected:
        /* 顶替太频繁（两端撞名互踢）：保留在位的那个，断开新来的这个。 */
        LogErrorf(logger_, "RTMP publish rejected for key=\"%s\" (keep=%s new=%s, "
                  "too many takeovers)", req_.key_.c_str(),
                  claim.previous_owner.c_str(), id_.c_str());
        return false;
    case PublishClaim::kReused:
        LogInfof(logger_, "RTMP publish reused key=\"%s\" (owner=%s)",
                 req_.key_.c_str(), id_.c_str());
        break;
    default:
        LogInfof(logger_, "RTMP publish created key=\"%s\" (owner=%s)",
                 req_.key_.c_str(), id_.c_str());
        break;
    }
    claimed_ = true;
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
    else if (req_.publish_flag_ && claimed_ && !req_.key_.empty()) {
        /* ⚠️ 必须带 owner。被顶掉的旧会话在关闭时也会走到这里 —— 无条件
         * RemovePublisher 会把新 publisher 刚建立的 GOP 缓存清掉（实测最容易
         * 出现在 OBS 重连的竞态里）。ReleasePublisher 对非 owner 是 no-op。 */
        if (MediaStreamManager::ReleasePublisher(req_.key_, id_)) {
            LogInfof(logger_, "RTMP session %s released stream \"%s\"",
                     id_.c_str(), req_.key_.c_str());
        } else {
            LogInfof(logger_, "RTMP session %s: stream \"%s\" not released "
                     "(no longer the publisher)", id_.c_str(), req_.key_.c_str());
        }
        claimed_ = false;
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
            /* owner 校验：只有已认领、且仍是当前 publisher 的会话才允许写。
             * 被顶掉的旧会话在这里丢帧（metadata 也走同一道门）—— 否则两条
             * 时间线会混进同一条流，且 GOP 缓存会被反复清空。 */
            if (claimed_ && MediaStreamManager::IsPublisherOwner(req_.key_, id_)) {
                /* 序列头先喂 catalog 合成器，再原样送给 MediaStreamManager
                 * （后者还要把它放进 GOP 缓存，供 HTTP-FLV / RTMP play 回放）。 */
                if (pkt_ptr->is_seq_hdr_) {
                    UpdateCatalogFromSeqHdr(pkt_ptr);
                } else if (catalog_pending_ &&
                           GetNowMilliSec() - catalog_pending_ms_ >= kCatalogCoalesceMs) {
                    /* 单轨流（或对端没发第二条 seq header）：用第一帧把 catalog
                     * 落下去，别让订阅方一直等。 */
                    FlushCatalog();
                }
                MediaStreamManager::WriterMediaPacket(pkt_ptr);
            } else {
                const int64_t now_s = GetNowMilliSec() / 1000;
                if (owner_drop_warn_s_ != now_s) {
                    owner_drop_warn_s_ = now_s;
                    LogWarnf(logger_, "RTMP session %s dropping media for key=\"%s\": "
                             "superseded by another publisher (or publish not ready)",
                             id_.c_str(), req_.key_.c_str());
                }
            }

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