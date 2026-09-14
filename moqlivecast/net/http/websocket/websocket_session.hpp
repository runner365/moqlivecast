#ifndef WEBSOCKET_SESSION_HPP
#define WEBSOCKET_SESSION_HPP

#include "tcp_session.hpp"
#include "utils/data_buffer.hpp"
#include "utils/logger.hpp"
#include "utils/timer.hpp"
#include "websocket_frame.hpp"
#include "ws_session_base.hpp"

#include <stdint.h>
#include <stddef.h>
#include <string>
#include <memory>
#include <vector>
#include <map>

namespace cpp_streamer
{
class WebSocketServer;
class WebSocketSession : public TcpSessionCallbackI, public WebSocketSessionBase, public TimerInterface
{
public:
    WebSocketSession(bool is_client, uv_loop_t* loop, uv_stream_t* handle, WebSocketServer* server, Logger* logger);
    WebSocketSession(bool is_client, uv_loop_t* loop, uv_stream_t* handle, WebSocketServer* server,
                    const std::string& key_file, const std::string& cert_file, Logger* logger);
    virtual ~WebSocketSession();

public:
    std::string GetRemoteAddress();
    void SetSessionCallback(WebSocketSessionCallBackI* cb);
    int64_t GetLastPongMs();
    const std::string& GetPath() const { return path_; }
    const std::map<std::string, std::string>& GetQueryMap() const { return query_map_; }
    void CloseSession();
    void AddHeader(const std::string& key, const std::string& value) {
        response_headers_[key] = value;
    }
    uv_loop_t* UvLoop() {
        return loop_;
	}
protected:
    virtual bool OnTimer() override;

protected:
    virtual void OnWrite(int ret_code, size_t sent_size) override;
    virtual void OnRead(int ret_code, const char* data, size_t data_size) override;

protected:
    virtual void HandleWsData(uint8_t* data, size_t len, int op_code) override;
    virtual void SendWsFrame(const uint8_t* data, size_t len, uint8_t op_code) override;
    virtual void HandleWsClose(uint8_t* data, size_t len) override;

private:
    void Init();
    int OnHandleHttpRequest();
    void SendHttpResponse();
    void SendErrorResponse();
    void OnHandleFrame(const uint8_t* data, size_t data_size);
    std::string GenHashcode();
    void GetPathAndQuery(const std::string& all_path, std::string& path, std::map<std::string, std::string>& query_map);

private:
	uv_loop_t* loop_ = nullptr;
    WebSocketServer* server_    = nullptr;
    Logger* logger_             = nullptr;
    std::unique_ptr<TcpSession> session_;
    std::string remote_addr_;

private:
    bool http_request_ready_ = false;
    DataBuffer http_recv_buffer_;

private:
    std::string method_;
    std::string path_;
    std::map<std::string, std::string> query_map_;
    std::map<std::string, std::string> headers_;
    std::map<std::string, std::string> response_headers_;
    int sec_ws_ver_ = 13;
    std::string sec_ws_key_;
    std::string sec_ws_protocol_;

private:
    std::string hash_code_;

private:
    WebSocketFrame frame_;
    std::vector<std::shared_ptr<DataBuffer>> recv_buffer_vec_;
    int die_count_ = 0;

private:
    WebSocketSessionCallBackI* cb_ = nullptr;
};
}

#endif