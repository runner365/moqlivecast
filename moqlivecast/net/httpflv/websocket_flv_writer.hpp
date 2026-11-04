#ifndef WEBSOCKET_FLV_WRITER_HPP
#define WEBSOCKET_FLV_WRITER_HPP

#include "media_packet.hpp"
#include "net/http/websocket/websocket_pub.hpp"
#include "utils/logger.hpp"

#include <memory>
#include <string>
#include <vector>

namespace cpp_streamer {

class WebSocketSession;

/* Writes the same FLV byte stream as HttpFlvWriter, but each FLV tag is a
 * WebSocket binary message.  MediaStreamManager feeds it the FLV tag body
 * (video: 5-byte FLV header + AVCC; audio: 2-byte FLV header + raw AAC). */
class WebSocketFlvWriter : public AvWriterInterface,
                           public WebSocketSessionCallBackI {
public:
    WebSocketFlvWriter(std::string key, std::string id,
                       std::shared_ptr<WebSocketSession> session,
                       Logger* logger);
    virtual ~WebSocketFlvWriter();

    int WritePacket(Media_Packet_Ptr pkt) override;
    std::string GetKey() override;
    std::string GetWriterId() override;
    void CloseWriter() override;
    bool IsInited() override;
    void SetInitFlag(bool flag) override;

    bool IsAlive();

    void OnReadData(int code, const uint8_t* data, size_t len) override;
    void OnReadText(int code, const std::string& text) override;
    void OnClose(int code, const std::string& desc) override;

private:
    int AppendFlvHeader(std::vector<uint8_t>& out);

private:
    std::string key_;
    std::string writer_id_;
    std::weak_ptr<WebSocketSession> session_;
    Logger* logger_ = nullptr;

    bool init_flag_ = false;
    bool flv_header_ready_ = false;
    bool closed_flag_ = false;
    int64_t alive_ms_ = -1;
};

} /* namespace cpp_streamer */

#endif /* WEBSOCKET_FLV_WRITER_HPP */
