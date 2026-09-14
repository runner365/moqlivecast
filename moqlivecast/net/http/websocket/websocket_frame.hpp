#ifndef WEBSOCKET_FRAME_HPP
#define WEBSOCKET_FRAME_HPP
#include "websocket_pub.hpp"
#include "utils/data_buffer.hpp"
#include <stdint.h>

namespace cpp_streamer
{
class WebSocketFrame
{
public:
    WebSocketFrame();
    ~WebSocketFrame();

public:
    int Parse(const uint8_t* data, size_t len);
    int GetPayloadStart();
    int64_t GetPayloadLen();
    uint8_t* GetPayloadData();
    size_t GetBufferLen();
    bool PayloadIsReady();
    uint8_t* Consume(size_t len);
    uint8_t GetOperCode();
    bool GetFin();
    bool IsHeaderReady();
    void Reset();

private:
    DataBuffer buffer_;
    int payload_start_   = 0;
    int64_t payload_len_ = 0;
    uint8_t opcode_      = 0;
    bool mask_enable_    = false;
    bool fin_            = false;
    bool header_ready_   = false;
    uint8_t masking_key_[4];
};
}

#endif