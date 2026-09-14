#ifndef UDP_PUB_HPP
#define UDP_PUB_HPP
#include "logger.hpp"
#include "data_buffer.hpp"
#include "ipaddress.hpp"
#include <sstream>
#include <memory>
#include <string>
#include <stdint.h>
#include <iostream>
#include <queue>
#include <uv.h>

namespace cpp_streamer
{

#define UDP_DATA_BUFFER_MAX (10*1024)

class UdpSessionCallbackI;
typedef struct UdpReqInfoS
{
    uv_udp_send_t handle;
    uv_buf_t buf;
    char ip[16];
    uint16_t port;
    UdpSessionCallbackI* user_cb;
} UdpReqInfo;

class UdpServer;

inline void UdpAllocCallback(uv_handle_t* handle,
                    size_t suggested_size,
                    uv_buf_t* buf);
inline void UdpReadCallback(uv_udp_t* handle,
                    ssize_t nread,
                    const uv_buf_t* buf,
                    const struct sockaddr* addr,
                    unsigned flags);
inline void UdpSendCallback(uv_udp_send_t* req, int status);
inline void UdpCloseCallback(uv_handle_t* handle);

class UdpTuple
{
public:
    UdpTuple() {
    }
    UdpTuple(const std::string& ip, uint16_t udp_port): ip_address(ip)
        , port(udp_port)
    {
        struct sockaddr_in addr;
        uv_ip4_addr(ip.c_str(), udp_port, &addr);
        addr_u64_ = *((uint64_t*)&addr);
    }
    ~UdpTuple(){
    }

    std::string to_string() const {
        std::string ret = ip_address;

        ret += ":";
        ret += std::to_string(port);
        return ret;
    }
    uint64_t to_u64() const {
        return addr_u64_;
    }
public:
    std::string ip_address;
    uint16_t    port = 0;

private:
    uint64_t addr_u64_ = 0;
};

class UdpSessionCallbackI
{
public:
    virtual void OnWrite(size_t sent_size, UdpTuple address) = 0;
    virtual void OnRead(const char* data, size_t data_size, UdpTuple address) = 0;
};

class UdpSessionBase
{
friend void UdpAllocCallback(uv_handle_t* handle,
                    size_t suggested_size,
                    uv_buf_t* buf);
friend void UdpReadCallback(uv_udp_t* handle,
                    ssize_t nread,
                    const uv_buf_t* buf,
                    const struct sockaddr* addr,
                    unsigned flags);
friend void UdpSendCallback(uv_udp_send_t* req, int status);

public:
    UdpSessionBase(uv_loop_t* loop, 
                UdpSessionCallbackI* cb, 
                Logger* logger): loop_(loop)
                                , cb_(cb)
                                , logger_(logger)
    {
    }
    ~UdpSessionBase()
    {
        Close();
    }

public:
    uv_loop_t* GetLoop() { return loop_; }

    std::string GetLocalAddress(uint16_t& port) {
        std::string ip;
        struct sockaddr addr;
        int addr_len = sizeof(addr);

        int ret = uv_udp_getsockname(udp_handle_, &addr, &addr_len);
        if (ret != 0) {
            return ip;
        }
        ip = GetIpStr(&addr, port);
        return ip;
    }
    
    void Write(const char* data, size_t len, UdpTuple remote_address) {
        struct sockaddr_in send_addr;
        UdpReqInfo* req = (UdpReqInfo*)malloc(sizeof(UdpReqInfo));
        uv_ip4_addr(remote_address.ip_address.c_str(), remote_address.port, &send_addr);

        /* Store the session pointer in req->handle.data so the completion
         * callback can remove the request from the pending set while the
         * session is still alive. If the session is being closed/destroyed
         * we will clear req->handle.data in Close() so callbacks won't
         * dereference freed memory.
         */
        req->handle.data = this;
        req->user_cb = cb_;

        char* new_data = (char*)malloc(len);
        memcpy(new_data, data, len);
        req->buf = uv_buf_init(new_data, (unsigned int)len);

        memset(req->ip, 0, sizeof(req->ip));
        memcpy(req->ip, remote_address.ip_address.c_str(), remote_address.ip_address.size());
        req->port = remote_address.port;

        uv_udp_send((uv_udp_send_t*)req, udp_handle_, &req->buf, 1,
            (const struct sockaddr *)&send_addr, UdpSendCallback);
    }

    void TryRead() {
        int ret = 0;
        ret = uv_udp_recv_start(udp_handle_, UdpAllocCallback, UdpReadCallback);
        if (ret != 0) {
            if (ret == UV_EALREADY) {
                return;
            }
            std::stringstream err_ss;
            err_ss << "uv_udp_recv_start error:" << ret;
            throw CppStreamException(err_ss.str().c_str());
        }
    }

    void Close() {
        if (close_flag_) {
            return;
        }
        close_flag_ = true;
        cb_ = nullptr;
        uv_udp_recv_stop(udp_handle_);
        /* Prevent future read callbacks from accessing this object. */
        udp_handle_->data = nullptr;

        uv_close((uv_handle_t*)udp_handle_, UdpCloseCallback);
    }

protected:
    void OnAlloc(uv_buf_t* buf) {
        buf->base = recv_buffer_;
        buf->len  = UDP_DATA_BUFFER_MAX;
    }

    void OnRead(uv_udp_t* handle,
            ssize_t nread,
            const uv_buf_t* buf,
            const struct sockaddr* addr,
            unsigned flags) {
        if (close_flag_) {
            return;
        }
        if (cb_) {
            if (nread > 0) {
                uint16_t remote_port = 0;
                std::string remote_ip = GetIpStr(addr, remote_port);
                remote_port = htons(remote_port);
                
                UdpTuple addr_tuple(remote_ip, remote_port);
                cb_->OnRead(buf->base, nread, addr_tuple);
            }
        }
        TryRead();
    }

    void OnWrite(uv_udp_send_t* req, int status) {
        UdpReqInfo* wr = (UdpReqInfo*)req;
        UdpTuple addr;

        /* Only call user callback if session is not closing. Always free
         * the request memory to avoid leaks.
         */
        bool do_callback = (!close_flag_ && cb_ != nullptr);

        if (status != 0) {
            if (do_callback) {
                cb_->OnWrite(0, addr);
            }
        } else {
            if (do_callback && wr) {
                addr.ip_address = wr->ip;
                addr.port       = wr->port;
                cb_->OnWrite(wr->buf.len, addr);
            }
        }

        if (wr) {
            if (wr->buf.base) {
                free(wr->buf.base);
            }
            free(wr);
        }
    }

protected:
    uv_loop_t* loop_         = nullptr;
    UdpSessionCallbackI* cb_ = nullptr;
    Logger* logger_          = nullptr;
    uv_udp_t* udp_handle_    = nullptr;
    bool close_flag_ = false;

protected:
    char recv_buffer_[UDP_DATA_BUFFER_MAX];
};

inline void UdpAllocCallback(uv_handle_t* handle,
                    size_t suggested_size,
                    uv_buf_t* buf) {
    UdpSessionBase* session = (UdpSessionBase*)handle->data;
    if (session) {
        session->OnAlloc(buf);
    }
}

inline void UdpReadCallback(uv_udp_t* handle,
                    ssize_t nread,
                    const uv_buf_t* buf,
                    const struct sockaddr* addr,
                    unsigned flags) {
    UdpSessionBase* session = (UdpSessionBase*)handle->data;
    if (session) {
        session->OnRead(handle, nread, buf, addr, flags);
    }
}

inline void UdpSendCallback(uv_udp_send_t* req, int status) {
    UdpReqInfo* wr = (UdpReqInfo*)req;

    if (!wr) return;

    UdpTuple addr;
    addr.ip_address = wr->ip;
    addr.port = wr->port;

    if (wr->user_cb) {
        if (status != 0) {
            wr->user_cb->OnWrite(0, addr);
        } else {
            wr->user_cb->OnWrite(wr->buf.len, addr);
        }
    }

    if (wr->buf.base) free(wr->buf.base);
    free(wr);
}
inline void UdpCloseCallback(uv_handle_t* handle) {
    free(handle);
}

}

#endif //UDP_PUB_HPP
