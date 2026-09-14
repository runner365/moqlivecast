#include "ws_session_base.hpp"
#include "utils/stringex.hpp"
#include "utils/byte_stream.hpp"
#include "utils/timeex.hpp"

namespace cpp_streamer
{
WebSocketSessionBase::WebSocketSessionBase(Logger* logger):logger_(logger)
{
    frame_.reset(new WebSocketFrame());
    last_recv_pong_ms_ = now_millisec();
    last_send_ping_ms_ = now_millisec();
}

WebSocketSessionBase::~WebSocketSessionBase()
{
}

void WebSocketSessionBase::AsyncWriteText(const std::string& text) {
    SendWsFrame((const uint8_t*)text.c_str(), text.length(), WS_OP_TEXT_TYPE);
}

void WebSocketSessionBase::AsyncWriteData(const uint8_t* data, size_t len) {
    SendWsFrame(data, len, WS_OP_BIN_TYPE);
}

Logger* WebSocketSessionBase::GetLogger() {
    return logger_;
}

int WebSocketSessionBase::HandleFrame(DataBuffer& data) {
    int ret = 0;
    int i   = 0;

    do {
        if (i == 0) {
            ret = frame_->Parse((uint8_t*)data.Data(), data.DataLen());
            if (ret != 0) {
                return ret;
            }
        } else {
            ret = frame_->Parse(nullptr, 0);
            if (ret != 0) {
                return ret;
            }
        }
        i++;
        if (frame_->GetOperCode() == WS_OP_TEXT_TYPE || frame_->GetOperCode() == WS_OP_BIN_TYPE) {
            last_op_code_ = frame_->GetOperCode();
        }
        
        if (!frame_->PayloadIsReady()) {
            return 1;
        }
        std::shared_ptr<DataBuffer> buffer_ptr = std::make_shared<DataBuffer>(frame_->GetPayloadLen() + 1024);
        buffer_ptr->AppendData((char*)frame_->GetPayloadData(), frame_->GetPayloadLen());
        recv_buffer_vec_.emplace_back(std::move(buffer_ptr));

        frame_->Consume(frame_->GetPayloadStart() + frame_->GetPayloadLen());
        frame_->Reset();

        if (!frame_->GetFin()) {
            continue;
        }
        die_count_ = 0;
        
        switch (frame_->GetOperCode())
        {
            case WS_OP_PING_TYPE:
            {
                HandleWsPing();
                break;
            }
            case WS_OP_PONG_TYPE:
            {
                LogDebugf(logger_, "receive ws pong");
                last_recv_pong_ms_ = now_millisec();
                break;
            }
            case WS_OP_CLOSE_TYPE:
            {
                for (auto item : recv_buffer_vec_) {
                    HandleWsClose((uint8_t*)item->Data(), item->DataLen());
                }
                break;
            }
            case WS_OP_CONTINUE_TYPE:
            {
                for (auto item : recv_buffer_vec_) {
                    HandleWsData((uint8_t*)item->Data(), item->DataLen(), last_op_code_);
                }
                break;
            }
            case WS_OP_TEXT_TYPE:
            case WS_OP_BIN_TYPE:
            {
                for (auto item : recv_buffer_vec_) {
                    HandleWsData((uint8_t*)item->Data(), item->DataLen(), frame_->GetOperCode());
                }
                break;
            }
            default:
                LogErrorf(logger_, "websocket opcode:%d not handle", frame_->GetOperCode());
                break;
        }
        recv_buffer_vec_.clear();
    } while(frame_->GetBufferLen() > 0);
    return 0;
}

void WebSocketSessionBase::HandleWsPing() {
    for (auto item : recv_buffer_vec_) {
        SendWsFrame((uint8_t*)item->Data(), item->DataLen(), WS_OP_PONG_TYPE);
    }
}

void WebSocketSessionBase::SendClose(uint16_t code, const char *reason) {
    size_t reason_len = strlen(reason);
    SendWsFrame((uint8_t*)reason, reason_len, WS_OP_CLOSE_TYPE);
}

void WebSocketSessionBase::SendPingFrame(int64_t now_ms) {
    int64_t now_sec = now_ms / 1000;
    uint8_t data[4];
    
    ByteStream::Write4Bytes(data, (uint32_t)now_sec);
    SendWsFrame(data, sizeof(data), WS_OP_PING_TYPE);
}

}