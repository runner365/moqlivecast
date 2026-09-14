#ifndef CO_HTTP_SERVER_HPP
#define CO_HTTP_SERVER_HPP
#ifdef _WIN64
#define WIN32_LEAN_AND_MEAN  // ���� Windows �ɰ�����ͷ�ļ������� winsock.h��
#endif
#include "utils/logger.hpp"
#include "utils/co_pub.hpp"
#include "utils/timer.hpp"
#include "net/tcp/co_tcp/co_tcp_server/co_tcp_server.hpp"
#include "co_http_session.hpp"
#include <memory>
#include <string>
#include <uv.h>
#include <unordered_map>

namespace cpp_streamer
{
class CoHttpServer : public CoHttpCallbackI, public TimerInterface
{
public:
    CoHttpServer(uv_loop_t* loop, const std::string& host, uint16_t port, Logger* logger);
    virtual ~CoHttpServer();

public:
    CoVoidTask Run();

public:
    virtual void OnClose(const std::string& endpoint) override;
    virtual CO_HTTP_HANDLE_PTR GetHandle(std::shared_ptr<CoHttpRequest> request) override;

protected:
    virtual bool OnTimer() override;

public:
    void AddGetHandle(const std::string& uri, CO_HTTP_HANDLE_PTR handle_func);
    void AddPostHandle(const std::string& uri, CO_HTTP_HANDLE_PTR handle_func);

private:
    Logger* logger_ = nullptr;
    std::shared_ptr<CoTcpServer> tcp_server_ptr_;

private:
    std::unordered_map<std::string, std::shared_ptr<CoHttpSession>> http_sessions_;
    std::unordered_map< std::string, CO_HTTP_HANDLE_PTR > get_handle_map_;
    std::unordered_map< std::string, CO_HTTP_HANDLE_PTR > post_handle_map_;
};

}

#endif // CO_HTTP_SERVER_HPP