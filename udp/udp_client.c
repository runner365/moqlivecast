#include "udp_client.h"
#include <stdlib.h>
#include <string.h>

#define UDP_BUF_SIZE 65536

/* ============================================
 * 发送上下文 — 携带 user callback
 * ============================================ */
typedef struct {
    uv_udp_send_t req;
    UdpClient    *client;
    size_t        data_len;
    char          data[];   /* 复制数据，确保异步发送时缓冲区有效 */
} SendContext;

/* ============================================
 * 内部 — 分配接收缓冲区
 * ============================================ */
static void on_alloc(uv_handle_t *handle, size_t suggested_size, uv_buf_t *buf) {
    (void)handle;
    *buf = uv_buf_init((char*)malloc(suggested_size), (unsigned int)suggested_size);
}

/* ============================================
 * 内部 — libuv 发送完成回调
 * ============================================ */
static void on_send_done(uv_udp_send_t *req, int status) {
    SendContext *ctx = (SendContext*)req;
    if (ctx->client->on_send_) {
        ctx->client->on_send_(ctx->client, status);
    }
    free(ctx);
}

/* ============================================
 * 内部 — libuv 接收回调
 * ============================================ */
static void on_recv_cb(uv_udp_t *handle, ssize_t nread, const uv_buf_t *buf,
                       const struct sockaddr *addr, unsigned flags) {
    (void)flags;
    UdpClient *client = (UdpClient*)handle->data;

    if (nread > 0 && client->on_recv_) {
        char from_ip[INET6_ADDRSTRLEN] = {0};
        int  from_port = 0;

        if (addr) {
            if (addr->sa_family == AF_INET) {
                struct sockaddr_in *s = (struct sockaddr_in*)addr;
                uv_ip4_name(s, from_ip, sizeof(from_ip));
                from_port = ntohs(s->sin_port);
            } else if (addr->sa_family == AF_INET6) {
                struct sockaddr_in6 *s = (struct sockaddr_in6*)addr;
                uv_ip6_name(s, from_ip, sizeof(from_ip));
                from_port = ntohs(s->sin6_port);
            }
        }

        client->on_recv_(client, buf->base, nread, from_ip, from_port);
    }

    if (buf->base) {
        free(buf->base);
    }
}

/* ============================================
 * 内部 — uv_close 回调，释放全部资源
 * ============================================ */
static void on_close_cb(uv_handle_t *handle) {
    UdpClient *client = (UdpClient*)handle->data;

    UdpClientOnClose user_cb = client->on_close_;

    /* 先回调，client 指针仍有效 */
    if (user_cb) {
        user_cb(client);
    }

    free(client->buffer_);
    free(client->remote_ip_);
    free(client->udp_);
    free(client);
}

/* ============================================
 * 公有 — 构造
 * ============================================ */
UdpClient* UdpClientConstruct(uv_loop_t *loop) {
    UdpClient *client = (UdpClient*)calloc(1, sizeof(UdpClient));
    if (!client) return NULL;

    client->loop_        = loop;
    client->buffer_      = (char*)malloc(UDP_BUF_SIZE);
    client->buffer_size_ = UDP_BUF_SIZE;
    client->udp_         = (uv_udp_t*)malloc(sizeof(uv_udp_t));

    if (!client->buffer_ || !client->udp_) {
        free(client->buffer_);
        free(client->udp_);
        free(client);
        return NULL;
    }

    uv_udp_init(loop, client->udp_);
    client->udp_->data = client;

    return client;
}

/* ============================================
 * 公有 — 析构 (异步，资源在 on_close_cb 中释放)
 * ============================================ */
void UdpClientDestruct(UdpClient* client) {
    if (!client) return;
    if (client->udp_ && !uv_is_closing((uv_handle_t*)client->udp_)) {
        uv_close((uv_handle_t*)client->udp_, on_close_cb);
    } else {
        /* handle 不存在或已在关闭中，直接释放 */
        free(client->buffer_);
        free(client->remote_ip_);
        free(client->udp_);
        free(client);
    }
}

/* ============================================
 * 公有 — 设置回调
 * ============================================ */
void UdpClientSetOnSend(UdpClient* client, UdpClientOnSend cb) {
    client->on_send_ = cb;
}

void UdpClientSetOnRecv(UdpClient* client, UdpClientOnRecv cb) {
    client->on_recv_ = cb;
}

void UdpClientSetOnClose(UdpClient* client, UdpClientOnClose cb) {
    client->on_close_ = cb;
}

/* ============================================
 * 公有 — 绑定本地地址
 * ============================================ */
int UdpClientBind(UdpClient* client, const char* ip, int port) {
    struct sockaddr_storage addr;
    int ret;

    if (strchr(ip, ':')) {
        struct sockaddr_in6 *a6 = (struct sockaddr_in6*)&addr;
        ret = uv_ip6_addr(ip, port, a6);
    } else {
        struct sockaddr_in *a4 = (struct sockaddr_in*)&addr;
        ret = uv_ip4_addr(ip, port, a4);
    }
    if (ret < 0) return ret;

    return uv_udp_bind(client->udp_, (const struct sockaddr*)&addr, 0);
}

/* ============================================
 * 公有 — 启动接收
 * ============================================ */
int UdpClientStartRecv(UdpClient* client) {
    return uv_udp_recv_start(client->udp_, on_alloc, on_recv_cb);
}

/* ============================================
 * 公有 — 发送数据
 * ============================================ */
int UdpClientSend(UdpClient* client, const char* data, size_t length,
                  const char* ip, int port) {
    struct sockaddr_storage addr;
    int ret;

    if (strchr(ip, ':')) {
        struct sockaddr_in6 *a6 = (struct sockaddr_in6*)&addr;
        ret = uv_ip6_addr(ip, port, a6);
    } else {
        struct sockaddr_in *a4 = (struct sockaddr_in*)&addr;
        ret = uv_ip4_addr(ip, port, a4);
    }
    if (ret < 0) return ret;

    SendContext *ctx = (SendContext*)malloc(sizeof(SendContext) + length);
    if (!ctx) return UV_ENOMEM;

    ctx->client    = client;
    ctx->data_len  = length;
    memcpy(ctx->data, data, length);

    uv_buf_t buf = uv_buf_init(ctx->data, (unsigned int)length);

    ret = uv_udp_send(&ctx->req, client->udp_, &buf, 1,
                      (const struct sockaddr*)&addr, on_send_done);
    if (ret < 0) {
        free(ctx);
    }
    return ret;
}
