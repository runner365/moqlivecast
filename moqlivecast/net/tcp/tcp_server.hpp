#ifndef TCP_SERVER_HPP
#define TCP_SERVER_HPP
#ifdef _WIN64
#define WIN32_LEAN_AND_MEAN  // ���� Windows �ɰ�����ͷ�ļ������� winsock.h��
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

        /* ⚠️ 句柄必须**独立分配**，不能是 TcpServer 的内嵌成员。
         * uv_close() 只是把句柄挂进 loop 的 closing 队列，close 回调要等到
         * 本轮 uv_run 才执行；如果句柄跟着 TcpServer 一起析构，libuv 处理
         * closing 队列时读到的是已释放内存：
         *   Assertion failed: (handle->flags & UV_HANDLE_CLOSING)
         * 句柄的生命周期归 loop：Close() 之后由 on_uv_server_close 释放。 */
        server_handle_ = (uv_tcp_t*)calloc(1, sizeof(uv_tcp_t));
        if (!server_handle_) {
            init_err_ = UV_ENOMEM;
            bind_err_ = listen_err_ = init_err_;
            return;
        }

        /* ⚠️ 这三步的返回值**必须留下**。以前全部忽略，端口被占时 listen
         * 根本没成功，而日志里一句都没有 —— 上层只能靠“连不上”去猜。
         * 现在由调用方（如 RtmpServer::Ok）检查，启动期就能明确报错。 */
        init_err_ = uv_tcp_init(loop_, server_handle_);
        if (init_err_ != 0) {
            free(server_handle_);
            server_handle_ = nullptr;
            bind_err_ = listen_err_ = init_err_;
            return;
        }

        bind_err_ = uv_tcp_bind(server_handle_,
                                (const struct sockaddr*)&server_addr_, 0);
        if (bind_err_ != 0) {
            listen_err_ = bind_err_;
            return;
        }

        server_handle_->data = this;
        listen_err_ = uv_listen((uv_stream_t*)server_handle_, SOMAXCONN,
                                on_uv_connection);
    }

    virtual ~TcpServer()
    {
        Close();
    }

    /* listen 是否真的成功。0/0/0 才可用。 */
    bool Ok() const {
        return init_err_ == 0 && bind_err_ == 0 && listen_err_ == 0;
    }
    int InitError() const { return init_err_; }
    int BindError() const { return bind_err_; }
    int ListenError() const { return listen_err_; }

    void Close() {
        if (closed_) {
            return;
        }
        closed_ = true;
        if (!server_handle_) {
            return;   /* 分配 / uv_tcp_init 失败时已在构造里释放 */
        }
        if (init_err_ != 0) {
            /* uv_tcp_init 未成功：句柄从未注册进 loop，uv_close 是 UB，
             * 直接释放。 */
            free(server_handle_);
            server_handle_ = nullptr;
            return;
        }
        /* 关闭后不再派发 accept（in-flight 的连接事件会看到 data == nullptr）。 */
        server_handle_->data = nullptr;
        uv_close(reinterpret_cast<uv_handle_t*>(server_handle_),
                 static_cast<uv_close_cb>(on_uv_server_close));
        /* 所有权交给 uv：释放发生在 on_uv_server_close。这里必须置空，
         * 否则析构/重复 Close 会拿到一个已被 loop 接管的指针。 */
        server_handle_ = nullptr;
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
    uv_tcp_t* server_handle_ = nullptr;   /* calloc；由 uv close 回调释放 */
    struct sockaddr_in server_addr_;
    bool closed_                    = false;
    int init_err_                   = 0;
    int bind_err_                   = 0;
    int listen_err_                 = 0;
};

inline void on_uv_connection(uv_stream_t* handle, int status) {
    auto* server = static_cast<TcpServer*>(handle->data);
    if (server) {
        server->OnConnection(status, handle);
    }
}

inline void on_uv_server_close(uv_handle_t* handle) {
    /* server_handle_ 由构造函数 calloc，生命周期归 loop：只有这里、
     * uv_close 的回调里才释放。TcpServer 对象本身可以在此之前析构。 */
    free(handle);
}

}
#endif //TCP_SERVER_HPP