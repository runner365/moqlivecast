#ifndef TCP_SERVER_HPP
#define TCP_SERVER_HPP
#ifdef _WIN64
#define WIN32_LEAN_AND_MEAN  // ÆÁ±Î Windows ¾É°æÈßÓàÍ·ÎÄ¼þ£¨°üÀ¨ winsock.h£©
#endif
#include "tcp_session.hpp"
#include "tcp_pub.hpp"
#include <uv.h>
#include <memory>
#include <string>
#include <stdint.h>
#include <cstdlib>
#include <functional>
#include <iostream>

namespace cpp_streamer
{
class TcpServer;

inline void on_uv_connection(uv_stream_t* handle, int status);
inline void on_uv_server_close(uv_handle_t* handle);

class TcpServer
{
friend void on_uv_connection(uv_stream_t* handle, int status);
friend void on_uv_server_close(uv_handle_t* handle);

public:
    TcpServer(uv_loop_t* loop,
        const std::string& ip,
        uint16_t local_port,
        TcpServerCallbackI* callback):loop_(loop)
                                    , callback_(callback)
                                    , closed_(false)
    {
        uv_ip4_addr(ip.c_str(), local_port, &server_addr_);
        uv_tcp_init(loop_, &server_handle_);

        uv_tcp_bind(&server_handle_, (const struct sockaddr*)&server_addr_, 0);

        server_handle_.data = this;
        uv_listen((uv_stream_t*)&server_handle_, SOMAXCONN, on_uv_connection);
    }

    virtual ~TcpServer()
    {
        std::cout << "tcp server destruct...\r\n";
    }

    void Close() {
        if (closed_) {
            return;
        }
        closed_ = true;
        server_handle_.data = nullptr;

        uv_close(reinterpret_cast<uv_handle_t*>(&server_handle_), static_cast<uv_close_cb>(on_uv_server_close));
    }

private:
    void OnConnection(int status, uv_stream_t* handle) {
        if (callback_) {
            callback_->OnAccept(status, loop_, handle);
        }
    }

private:
    uv_loop_t* loop_                = nullptr;
    TcpServerCallbackI* callback_   = nullptr;
    uv_tcp_t server_handle_;
    struct sockaddr_in server_addr_;
    bool closed_                    = false;
};

inline void on_uv_connection(uv_stream_t* handle, int status) {
    auto* server = static_cast<TcpServer*>(handle->data);
    if (server) {
        server->OnConnection(status, handle);
    }
}

inline void on_uv_server_close(uv_handle_t* handle) {
    delete handle;
}

}
#endif //TCP_SERVER_HPP