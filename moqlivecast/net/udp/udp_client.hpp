#ifndef UDP_CLIENT_HPP
#define UDP_CLIENT_HPP
#include "udp_pub.hpp"
#include "logger.hpp"
#include <iostream>

namespace cpp_streamer
{

class UdpClient : public UdpSessionBase
{
public:
    UdpClient(uv_loop_t* loop, UdpSessionCallbackI* cb,
            Logger* logger,
            const char* ipaddr_sz = nullptr,
            uint16_t port = 0):UdpSessionBase(loop, 
                                            cb, 
                                            logger)
    {
        struct sockaddr_in recv_addr;
        udp_handle_ = (uv_udp_t*)malloc(sizeof(uv_udp_t));//it will be freed in Udp close callback
        memset(udp_handle_, 0, sizeof(uv_udp_t));
        uv_udp_init(loop, udp_handle_);

        if (ipaddr_sz == nullptr) {
            uv_ip4_addr("0.0.0.0", port, &recv_addr);
        } else {
            uv_ip4_addr(ipaddr_sz, port, &recv_addr);
        }
        uv_udp_bind(udp_handle_, (const struct sockaddr *)&recv_addr, UV_UDP_REUSEADDR);

        udp_handle_->data = this;
    }
    ~UdpClient()
    {
    }
};

}
#endif //UDP_CLIENT_HPP
