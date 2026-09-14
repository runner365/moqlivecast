#ifndef CO_HTTP_SESSION_HPP
#define CO_HTTP_SESSION_HPP
#ifdef _WIN64
#define WIN32_LEAN_AND_MEAN  // ÆÁ±Î Windows ¾É°æÈßÓàÍ·ÎÄ¼þ£¨°üÀ¨ winsock.h£©
#endif
#include "net/tcp/co_tcp/co_tcp_pub.hpp"
#include "net/tcp/co_tcp/co_tcp_server/co_tcp_accept_conn.hpp"
#include "utils/logger.hpp"
#include "utils/co_pub.hpp"
#include "utils/data_buffer.hpp"
#include "co_http_common.hpp"

#include <memory>

namespace cpp_streamer
{
class CoHttpSession : public HttpResponseCallbackI
{
public:
    CoHttpSession(std::shared_ptr<TcpCoAcceptConn> accept_conn, CoHttpCallbackI* cb, Logger* logger);
    virtual ~CoHttpSession();

public:
    CoVoidTask HandleRequest();
    bool IsAlive() const;

public:
    virtual void OnResponseAlive() override;
    virtual void OnResponseClose() override;

private:
    int AnalyzeHeader(const uint8_t* data, size_t data_size);

private:
    std::shared_ptr<TcpCoAcceptConn> accept_conn_;
    CoHttpCallbackI* cb_ = nullptr;
    Logger* logger_;

private:
    std::shared_ptr<DataBuffer> header_data_;
    std::shared_ptr<DataBuffer> content_data_;

private:
    bool header_is_ready_ = false;
    int content_start_ = -1; // Start position of http request content
    std::shared_ptr<CoHttpRequest> request_;
    std::shared_ptr<CoHttpResponse> response_;

private:
    int64_t alive_ms_ = -1;
    bool is_close_ = false; // Flag to indicate if the session is closed
};
}

#endif // CO_HTTP_SESSION_HPP