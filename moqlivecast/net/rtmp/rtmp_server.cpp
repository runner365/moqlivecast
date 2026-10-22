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
            int64_t idle_timeout_ms,
            Logger* logger) : TimerInterface(20)
	                        , logger_(logger)
                            , idle_timeout_ms_(idle_timeout_ms > 0 ? idle_timeout_ms : 15000)
{
    tcp_server_ = std::make_unique<TcpServer>(loop, host, port, this);
    if (!Ok()) {
        LogErrorf(logger_, "RTMP listen %s:%d failed: init=%d bind=%d listen=%d",
                  host.c_str(), port,
                  tcp_server_->InitError(), tcp_server_->BindError(),
                  tcp_server_->ListenError());
    }
    StartTimer();
}

RtmpServer::~RtmpServer()
{
    StopTimer();
    /* 先停监听，再拆会话：会话析构会 Close 各自的 TCP，并从这里摘掉
     * MediaStreamManager 的 publisher/player 登记。 */
    if (tcp_server_) {
        tcp_server_->Close();
    }
    sessions_.clear();
}

bool RtmpServer::Ok() const {
    return tcp_server_ && tcp_server_->Ok();
}

int RtmpServer::BindError() const {
    return tcp_server_ ? tcp_server_->BindError() : 0;
}

int RtmpServer::ListenError() const {
    return tcp_server_ ? tcp_server_->ListenError() : 0;
}

bool RtmpServer::KickSession(const std::string& owner_id) {
    if (owner_id.empty()) return false;
    auto it = sessions_.find(owner_id);
    if (it == sessions_.end()) return false;

    LogInfof(logger_, "RTMP session %s superseded by a new publisher, closing it",
             owner_id.c_str());
    /* Close 是幂等的；erase 之后对象析构会再走一次 Close（closed_flag_ 拦住）。 */
    it->second->Close();
    sessions_.erase(it);
    return true;
}

void RtmpServer::OnAccept(int ret_code, uv_loop_t* loop, uv_stream_t* handle) {
	if (ret_code < 0) {
		LogErrorf(logger_, "RTMP server accept failed with error code: %d", ret_code);
		return;
	}
	auto session_ptr = std::make_shared<RtmpSession>(loop, handle, this,
                                                     idle_timeout_ms_, logger_);
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