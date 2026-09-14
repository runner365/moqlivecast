#ifndef UDP_SERVER_H
#define UDP_SERVER_H
#include <uv.h>

typedef struct UdpServer UdpServer;

typedef void (*UdpServerOnRecv)(UdpServer* server, const char* data, ssize_t nread,
                                const char* from_ip, int from_port);
typedef void (*UdpServerOnSend)(UdpServer* server, int status);
typedef void (*UdpServerOnError)(UdpServer* server, int errcode);
typedef void (*UdpServerOnClose)(UdpServer* server);

struct UdpServer {
    uv_loop_t *loop_;
    uv_udp_t  *udp_;
    char      *buffer_;
    size_t     buffer_size_;

    UdpServerOnRecv  on_recv_;
    UdpServerOnSend  on_send_;
    UdpServerOnError on_error_;
    UdpServerOnClose on_close_;

    void *app_data_; /* 上层应用回指针 */
};

UdpServer* UdpServerConstruct(uv_loop_t *loop);
void       UdpServerDestruct(UdpServer* server);

void UdpServerSetOnRecv(UdpServer* server, UdpServerOnRecv cb);
void UdpServerSetOnSend(UdpServer* server, UdpServerOnSend cb);
void UdpServerSetOnError(UdpServer* server, UdpServerOnError cb);
void UdpServerSetOnClose(UdpServer* server, UdpServerOnClose cb);

int UdpServerBind(UdpServer* server, const char* ip, int port);
int UdpServerStartRecv(UdpServer* server);

int UdpServerSend(UdpServer* server, const char* data, size_t length,
                  const char* ip, int port);

#endif // UDP_SERVER_H
