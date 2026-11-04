#ifndef WEBSOCKET_FLV_SERVER_HPP
#define WEBSOCKET_FLV_SERVER_HPP

#include "net/http/websocket/websocket_server.hpp"
#include "utils/logger.hpp"
#include "utils/timer.hpp"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace cpp_streamer {

class WebSocketSession;
class WebSocketFlvWriter;

/* WSS/WS endpoint:
 *   wss://host:port/wsflv?app=live&stream=NAME
 * Each FLV tag is sent as one WebSocket binary message. */
class WebSocketFlvServer : public TimerInterface {
public:
    WebSocketFlvServer(uv_loop_t* loop, const std::string& ip, uint16_t port,
                       const std::string& key_file,
                       const std::string& cert_file,
                       Logger* logger);
    virtual ~WebSocketFlvServer();

private:
    void OnWsSession(const std::string& uri, WebSocketSession* session);
    void OnCheckAlive();
    bool OnTimer() override;

private:
    std::unique_ptr<WebSocketServer> server_;
    std::unordered_map<std::string, WebSocketFlvWriter*> writers_;
    /* Created during the HTTP upgrade handler; AddPlayer() must wait until
     * the 101 response has been written, otherwise the GOP replay writes
     * before the handshake response. */
    std::vector<WebSocketFlvWriter*> pending_players_;
    Logger* logger_ = nullptr;
};

} /* namespace cpp_streamer */

#endif /* WEBSOCKET_FLV_SERVER_HPP */
