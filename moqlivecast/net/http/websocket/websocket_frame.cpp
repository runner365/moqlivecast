#include "websocket_frame.hpp"

namespace cpp_streamer
{
WebSocketFrame::WebSocketFrame()
{
}

WebSocketFrame::~WebSocketFrame()
{
}

int WebSocketFrame::Parse(const uint8_t* data, size_t len) {
    if (data != nullptr && len > 0) {
        buffer_.AppendData((char *)data, len);
    }

    if (buffer_.DataLen() < 2) {
        return 1;
    }

    if (!header_ready_) {
        WS_PACKET_HEADER *header = (WS_PACKET_HEADER *)buffer_.Data();
        payload_len_ = header->payload_len;
        payload_start_ = 2;

        fin_    = header->fin;
        opcode_ = header->opcode;
        if (header->payload_len == 126 || header->payload_len == 127) {
            if (header->payload_len == 127) {
                int64_t ext_len = 0;

                if (buffer_.DataLen() < 10) {
                    return 1;
                }

                uint8_t* p = (uint8_t*)(header + 1);
                ext_len |= ((uint64_t)(*p++)) << 56;
                ext_len |= ((uint64_t)(*p++)) << 48;
                ext_len |= ((uint64_t)(*p++)) << 40;
                ext_len |= ((uint64_t)(*p++)) << 32;
                ext_len |= ((uint64_t)(*p++)) << 24;
                ext_len |= ((uint64_t)(*p++)) << 16;
                ext_len |= ((uint64_t)(*p++)) << 8;
                ext_len |= *p;

                payload_len_ = ext_len;
                payload_start_ += 8;
            }
            else {
                if (buffer_.DataLen() < 4) {
                    return 1;
                }
                uint8_t *p = (uint8_t *)(buffer_.Data() + payload_start_);
                payload_len_ = 0;
                payload_len_ |= ((uint16_t)*p) << 8;
                p++;
                payload_len_ |= *p;
                payload_start_ += 2;
            }
        }
        mask_enable_ = header->mask;
        if (mask_enable_) {
            if ((int)buffer_.DataLen() < (payload_start_ + 4)) {
                return 1;
            }
            uint8_t *p = (uint8_t*)buffer_.Data() + payload_start_;
            payload_start_ += 4;
            masking_key_[0] = p[0];
            masking_key_[1] = p[1];
            masking_key_[2] = p[2];
            masking_key_[3] = p[3];
        }
        header_ready_ = true;
    }

    if ((int)buffer_.DataLen() < (payload_start_ + payload_len_)) {
        return 1;
    }

    if (mask_enable_) {
        size_t frame_length = payload_len_;
        uint8_t *p = (uint8_t *)buffer_.Data() + payload_start_;

        size_t temp_len = frame_length & ~3;
        for (size_t i = 0; i < temp_len; i += 4) {
            p[i + 0] ^= masking_key_[0];
            p[i + 1] ^= masking_key_[1];
            p[i + 2] ^= masking_key_[2];
            p[i + 3] ^= masking_key_[3];
        }

        for (size_t i = temp_len; i < frame_length; ++i) {
            p[i] ^= masking_key_[i % 4];
        }
    }

    return 0;
}

int WebSocketFrame::GetPayloadStart() {
    return payload_start_;
}

int64_t WebSocketFrame::GetPayloadLen() {
    return payload_len_;
}

uint8_t* WebSocketFrame::GetPayloadData() {
    return (uint8_t*)buffer_.Data() + payload_start_;
}

size_t WebSocketFrame::GetBufferLen() {
    return buffer_.DataLen();
}

bool WebSocketFrame::PayloadIsReady() {
    return (int)payload_len_ <= (int)buffer_.DataLen() - payload_start_;
}
    
uint8_t* WebSocketFrame::Consume(size_t len) {
    return (uint8_t*)buffer_.ConsumeData((int)len);
}

uint8_t WebSocketFrame::GetOperCode() {
    return opcode_;
}

bool WebSocketFrame::GetFin() {
    return fin_;
}

bool WebSocketFrame::IsHeaderReady() {
    return header_ready_;
}

void WebSocketFrame::Reset() {
    payload_start_ = 0;
    payload_len_   = 0;
    header_ready_  = false;
}
}