#ifndef CO_RTMP_SESSION_HPP
#define CO_RTMP_SESSION_HPP
#ifdef _WIN64
#define WIN32_LEAN_AND_MEAN  // ���� Windows �ɰ�����ͷ�ļ������� winsock.h��
#endif
#include "utils/logger.hpp"
#include "utils/data_buffer.hpp"
#include "net/tcp/tcp_session.hpp"
#include "rtmp_pub.hpp"
#include "rtmp_handshake.hpp"
#include "rtmp_session_base.hpp"
#include "rtmp_control_handler.hpp"
#include "rtmp_request.hpp"
#include "media_packet.hpp"
#include <memory>
#include <string>

namespace cpp_streamer
{
class RtmpWriter;

class RtmpSession : public RtmpSessionBase, public TcpSessionCallbackI
{
public:
    RtmpSession(uv_loop_t* loop,
        uv_stream_t* server_uv_handle,
        Logger* logger);
    virtual ~RtmpSession();

public:
	std::string GetId() { return id_; }
    void Close();

public:
    bool IsAlive();
    std::string GetSessonKey();

public:
    int HandleRequest();

protected:
    virtual void OnWrite(int ret_code, size_t sent_size) override;
    virtual void OnRead(int ret_code, const char* data, size_t data_size) override;

public:
    virtual DataBuffer* GetRecvBuffer() override;
    virtual int RtmpSend(char* data, int len) override;
    virtual int RtmpSend(std::shared_ptr<DataBuffer> data_ptr) override;

private:
    int ReceiveChunkStream();
	
private:
    std::string id_;
    std::shared_ptr<TcpSession> conn_ptr_;
    Logger* logger_ = nullptr;

private:
    RTMP_SERVER_SESSION_PHASE phase_ = initial_phase;
    bool running_ = false;
    bool closed_flag_ = false;

private:
    RtmpServerHandshake hs_;
    RtmpControlHandler control_handler_;

private:
    int64_t alive_ms_ = -1;

private:
    RtmpWriter* play_writer_ = nullptr;
};

}
#endif // CO_RTMP_SESSION_HPP