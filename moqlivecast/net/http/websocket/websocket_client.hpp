#ifndef WEBSOCKET_CLIENT_HPP
#define WEBSOCKET_CLIENT_HPP
#include "net/http/http_client.hpp"
#include "utils/data_buffer.hpp"
#include "utils/timer.hpp"
#include "websocket_pub.hpp"
#include "websocket_frame.hpp"
#include "ws_session_base.hpp"
#include <memory>
#include <uv.h>

namespace cpp_streamer
{

class WebSocketClient : public HttpClientCallbackI, public TimerInterface, public WebSocketSessionBase
{
public:
    WebSocketClient(uv_loop_t* loop, const std::string& hostname, uint16_t port, const std::string& subpath, bool ssl_enable, Logger* logger, WebSocketConnectionCallBackI* conn_cb);
    virtual ~WebSocketClient();

public:
    void AsyncConnect(const std::map<std::string, std::string>& input_headers);

protected:
    virtual void OnHttpRead(int ret, std::shared_ptr<HttpClientResponse> resp_ptr) override;

protected:
    virtual bool OnTimer() override;

protected:
    virtual void HandleWsData(uint8_t* data, size_t len, int op_code) override;
    virtual void SendWsFrame(const uint8_t* data, size_t len, uint8_t op_code) override;
    virtual void HandleWsClose(uint8_t* data, size_t len) override;

private:
    void HandleHttpRespone(std::shared_ptr<HttpClientResponse> resp_ptr);

private:
    uv_loop_t*  loop_ = nullptr;
    std::string hostname_;
    uint16_t    port_ = 0;
    std::string subpath_;
    bool        ssl_enable_ = false;
    Logger*     logger_ = nullptr;
    WebSocketConnectionCallBackI* conn_cb_ = nullptr;
    std::unique_ptr<HttpClient> client_ptr_;

private:
    std::string key_;
    bool http_ready_ = false;
};

}

#endif
