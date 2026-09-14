#include "rtmp_server.hpp"
#include "rtmp_session.hpp"
#include "net/tcp/tcp_server.hpp"
#include "net/tcp/tcp_session.hpp"
#include "utils/uuid.hpp"
#include "utils/timeex.hpp"

namespace cpp_streamer
{
RtmpServer::RtmpServer(uv_loop_t* loop,
            const std::string& host,
            int port,
            Logger* logger) : TimerInterface(20)
	                        , logger_(logger)
{
    tcp_server_ = std::make_unique<TcpServer>(loop, host, port, this);
    StartTimer();
}

RtmpServer::~RtmpServer()
{
    StopTimer();
}

void RtmpServer::OnAccept(int ret_code, uv_loop_t* loop, uv_stream_t* handle) {
	if (ret_code < 0) {
		LogErrorf(logger_, "RTMP server accept failed with error code: %d", ret_code);
		return;
	}
	auto session_ptr = std::make_shared<RtmpSession>(loop, handle, logger_);
	sessions_[session_ptr->GetId()] = session_ptr;
}

bool RtmpServer::OnTimer() {
    int64_t now_ms = now_millisec();
    UpdateNowMilliSec(now_ms);
    OnCheckAlive(now_ms);
    return timer_running_;
}

void RtmpServer::OnCheckAlive(int64_t now_ms) {
    if (last_check_ms_ < 0) {
        last_check_ms_ = now_ms;
        return;
    }

    if (now_ms - last_check_ms_ < 100) {
        return;
    }
    last_check_ms_ = now_ms;

    for (auto it = sessions_.begin(); it != sessions_.end();) {
        if (!it->second->IsAlive()) {
            LogInfof(logger_, "RTMP session %s is no longer alive, closing it.", it->first.c_str());
            it->second->Close();
            it = sessions_.erase(it);
        } else {
            ++it;
        }
    }
}
}