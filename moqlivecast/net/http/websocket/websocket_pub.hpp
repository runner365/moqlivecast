#ifndef WEBSOCKET_PUB_HPP
#define WEBSOCKET_PUB_HPP
#include <stdint.h>
#include <string>

namespace cpp_streamer
{
#define WS_MAX_HEADER_LEN 10

#define WS_OP_CONTINUE_TYPE        0x00
#define WS_OP_TEXT_TYPE            0x01
#define WS_OP_BIN_TYPE             0x02
#define WS_OP_FURTHER_NO_CTRL_TYPE 0x03
#define WS_OP_CLOSE_TYPE           0x08
#define WS_OP_PING_TYPE            0x09
#define WS_OP_PONG_TYPE            0x0a
#define WS_OP_FURTHER_CTRL_TYPE    0x0b

typedef struct S_WS_PACKET_HEADER
{
    uint8_t opcode : 4;
    uint8_t rsv3 : 1;
    uint8_t rsv2 : 1;
    uint8_t rsv1 : 1;
    uint8_t fin : 1;

    uint8_t payload_len : 7;
    uint8_t mask : 1;
} WS_PACKET_HEADER;

std::string  GenWebSocketHashcode(const std::string& key);

class WebSocketSessionCallBackI
{
public:
    virtual void OnReadData(int code, const uint8_t* data, size_t len) = 0;
    virtual void OnReadText(int code, const std::string& text) = 0;
    virtual void OnClose(int code, const std::string& desc) = 0;
};

class WebSocketConnectionCallBackI : public WebSocketSessionCallBackI
{
public:
    virtual void OnConnection() = 0;
};

class WebSocketSession;
using HandleWebSocketPtr = void(*)(const std::string& uri, WebSocketSession* session);

}

#endif
