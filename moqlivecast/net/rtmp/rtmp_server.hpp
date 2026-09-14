#ifndef RTMP_SERVER_HPP
#define RTMP_SERVER_HPP
#ifdef _WIN64
#define WIN32_LEAN_AND_MEAN  // ���� Windows �ɰ�����ͷ�ļ������� winsock.h��
#endif
#include "utils/timer.hpp"
#include "utils/logger.hpp"
#include "net/tcp/tcp_server.hpp"
#include "rtmp_pub.hpp"
#include <memory>
#include <map>
#include <string>

namespace cpp_streamer
{

class RtmpSession;
class RtmpServer : public TimerInterface, public TcpServerCallbackI
{
public:
    RtmpServer(uv_loop_t* loop,
            const std::string& host,
            int port,
            Logger* logger);
    virtual ~RtmpServer();

protected:
    virtual bool OnTimer() override;

protected:
    virtual void OnAccept(int ret_code, uv_loop_t* loop, uv_stream_t* handle) override;

private:
    void OnCheckAlive(int64_t now_ms);

private:
    std::unique_ptr<TcpServer> tcp_server_;
    Logger* logger_ = nullptr;

private:
    std::map<std::string, std::shared_ptr<RtmpSession>> sessions_;
    int64_t last_check_ms_ = -1;
};
}

#endif