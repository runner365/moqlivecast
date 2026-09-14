#ifndef CO_HTTP_COMMON_HPP
#define CO_HTTP_COMMON_HPP
#ifdef _WIN64
#define WIN32_LEAN_AND_MEAN  // ÆÁ±Î Windows ¾É°æÈßÓàÍ·ÎÄ¼þ£¨°üÀ¨ winsock.h£©
#endif
#include "utils/data_buffer.hpp"
#include "utils/logger.hpp"
#include "utils/co_pub.hpp"
#include "net/tcp/co_tcp/co_tcp_pub.hpp"
#include "net/tcp/co_tcp/co_tcp_server/co_tcp_session_send.hpp"
#include "net/tcp/co_tcp/co_tcp_server/co_tcp_session_recv.hpp"
#include "net/tcp/co_tcp/co_tcp_server/co_tcp_accept_conn.hpp"

#include <string>
#include <map>
#include <vector>
#include <stdint.h>
#include <stddef.h>
#include <memory>

namespace cpp_streamer
{
int GetUriAndParams(const std::string& input, std::string& uri, std::map<std::string, std::string>& params);
std::string GetUri(std::string& uri);

class HttpResponseCallbackI
{
public:
    virtual void OnResponseAlive() = 0;
    virtual void OnResponseClose() = 0;
};

class CoHttpRequest
{
public:
    CoHttpRequest();
    ~CoHttpRequest();

public:
    std::string Dump();

public:
    std::string method_;
    std::string uri_;
    int content_length_ = 0; // Content length of the request
    std::map<std::string, std::string> params;
    std::string version_;
    std::map<std::string, std::string> headers_;

public:
    std::shared_ptr<DataBuffer> header_data_;
    std::shared_ptr<DataBuffer> content_data_;
};

class CoHttpResponse
{
public:
    CoHttpResponse(std::shared_ptr<TcpCoAcceptConn> accept_conn, HttpResponseCallbackI* cb, Logger* logger);
    ~CoHttpResponse();

public:
    void SetStatusCode(int status_code) { status_code_ = status_code; }
    void SetStatus(const std::string& status) { status_ = status; }
    void AddHeader(const std::string& key, const std::string& value) { headers_[key] = value; }
    std::map<std::string, std::string> Headers() { return headers_; }

public:
    CoTask<SendResult> Write(const char* data, size_t len, bool continue_flag = false);
	CoTask<SendResult> WriteStatus(uint32_t code, const std::string& status, bool continue_flag = false);

public:
    Logger* GetLogger() { return logger_; }
    void Close();

private:
    std::shared_ptr<TcpCoAcceptConn> accept_conn_;
    Logger* logger_ = nullptr;

private:
    bool is_close_ = false;
    bool written_header_ = false;
    std::string status_  = "OK";  // e.g. "OK"
    int status_code_     = 200;     // e.g. 200
    std::string proto_   = "HTTP";   // e.g. "HTTP"
    std::string version_ = "1.1"; // e.g. "1.1"
    std::map<std::string, std::string> headers_; //headers

private:
    HttpResponseCallbackI* cb_ = nullptr;
};

using CO_HTTP_HANDLE_PTR = CoVoidTask (*)(std::shared_ptr<CoHttpRequest> request, std::shared_ptr<CoHttpResponse> response_ptr);

class CoHttpCallbackI
{
public:
    virtual void OnClose(const std::string& endpoint) = 0;
    virtual CO_HTTP_HANDLE_PTR GetHandle(std::shared_ptr<CoHttpRequest> request) = 0;
};

}
#endif