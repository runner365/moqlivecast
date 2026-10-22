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
            int64_t idle_timeout_ms,
            Logger* logger);
    virtual ~RtmpServer();

    /* listen 是否真的成功（uv_tcp_init/bind/listen 三步都 0）。
     * TcpServer 以前吞掉这三个返回值，端口冲突时表现为“服务在跑但没人能连”。 */
    bool Ok() const;
    int BindError() const;
    int ListenError() const;

    /* 顶替时关闭旧的 RTMP 会话（owner id = 会话 id）。
     * 返回 false = 该 owner 不是 RTMP 会话（可能来自 moq-lite / MOQ）——
     * 那种情况下旧连接由它自己的 owner 校验停写、等 idle 超时回收。 */
    bool KickSession(const std::string& owner_id);

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
    /* 会话空闲超时（毫秒）：收发都没有活动超过这个时间就关闭。
     * 由 Config::RtmpIdleTimeoutMs() 传入，缺省 15s。 */
    int64_t idle_timeout_ms_ = 15000;
};
}

#endif