#ifndef UDP_CLIENT_H
#define UDP_CLIENT_H
#include <uv.h>

typedef struct UdpClient UdpClient;

typedef void (*UdpClientOnSend)(UdpClient* client, int status);
typedef void (*UdpClientOnRecv)(UdpClient* client, const char* data, ssize_t nread,
                                const char* from_ip, int from_port);
typedef void (*UdpClientOnClose)(UdpClient* client);

struct UdpClient {
    uv_loop_t *loop_;
    uv_udp_t  *udp_;
    char      *buffer_;
    size_t     buffer_size_;
    char      *remote_ip_;
    int        remote_port_;

    UdpClientOnSend  on_send_;
    UdpClientOnRecv  on_recv_;
    UdpClientOnClose on_close_;

    void *app_data_; /* 上层应用回指针 */
};

UdpClient* UdpClientConstruct(uv_loop_t *loop);
void       UdpClientDestruct(UdpClient* client);

void UdpClientSetOnSend(UdpClient* client, UdpClientOnSend cb);
void UdpClientSetOnRecv(UdpClient* client, UdpClientOnRecv cb);
void UdpClientSetOnClose(UdpClient* client, UdpClientOnClose cb);

int  UdpClientBind(UdpClient* client, const char* ip, int port);
int  UdpClientStartRecv(UdpClient* client);

int  UdpClientSend(UdpClient* client, const char* data, size_t length,
                   const char* ip, int port);

#endif // UDP_CLIENT_H
