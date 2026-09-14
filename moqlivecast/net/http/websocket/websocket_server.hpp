#ifndef WEBSOCKET_SERVER_HPP
#define WEBSOCKET_SERVER_HPP
#include "net/tcp/tcp_server.hpp"
#include "utils/logger.hpp"
#include "utils/timer.hpp"
#include "websocket_pub.hpp"

#include <memory>
#include <string>
#include <stdint.h>
#include <map>
#include <uv.h>

namespace cpp_streamer
{
class WebSocketSession;
class WebSocketServer : public TimerInterface, public TcpServerCallbackI
{
public:
    WebSocketServer(const std::string& ip, uint16_t port, uv_loop_t* loop, Logger* logger);
    WebSocketServer(const std::string& ip, uint16_t port, uv_loop_t* loop, const std::string& key_file, const std::string& cert_file, Logger* logger);
    virtual ~WebSocketServer();

public:
    void AddHandle(const std::string& uri, HandleWebSocketPtr handle_ptr);
    HandleWebSocketPtr GetHandle(const std::string& uri);

protected:
    virtual void OnAccept(int ret_code, uv_loop_t* loop, uv_stream_t* handle) override;

protected:
    virtual bool OnTimer() override;

private:
    uint16_t port_      = 0;
    uv_loop_t* loop_    = nullptr;
    Logger* logger_     = nullptr;
    std::string key_file_;
    std::string cert_file_;
    std::unique_ptr<TcpServer> server_ptr_  = nullptr;

private:
    std::map<std::string, HandleWebSocketPtr> uri_handles_;
    std::map<std::string, std::shared_ptr<WebSocketSession>> sessions_;
};

}
#endif
