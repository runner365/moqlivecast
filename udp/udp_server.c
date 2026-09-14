#include "udp_server.h"
#include <stdlib.h>
#include <string.h>

#define UDP_BUF_SIZE 65536

/* ============================================
 * 发送上下文
 * ============================================ */
typedef struct {
    uv_udp_send_t req;
    UdpServer    *server;
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
    if (ctx->server->on_send_) {
        ctx->server->on_send_(ctx->server, status);
    }
    free(ctx);
}

/* ============================================
 * 内部 — libuv 接收回调
 * ============================================ */
static void on_recv_cb(uv_udp_t *handle, ssize_t nread, const uv_buf_t *buf,
                       const struct sockaddr *addr, unsigned flags) {
    (void)flags;
    UdpServer *server = (UdpServer*)handle->data;

    if (nread < 0) {
        if (server->on_error_) {
            server->on_error_(server, (int)nread);
        }
        uv_udp_recv_stop(handle);
        goto cleanup;
    }

    if (nread > 0 && server->on_recv_) {
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

        server->on_recv_(server, buf->base, nread, from_ip, from_port);
    }

cleanup:
    if (buf->base) {
        free(buf->base);
    }
}

/* ============================================
 * 内部 — uv_close 回调，释放全部资源
 * ============================================ */
static void on_close_cb(uv_handle_t *handle) {
    UdpServer *server = (UdpServer*)handle->data;

    UdpServerOnClose user_cb = server->on_close_;

    /* 先回调，server 指针仍有效 */
    if (user_cb) {
        user_cb(server);
    }

    free(server->buffer_);
    free(server->udp_);
    free(server);
}

/* ============================================
 * 公有 — 构造
 * ============================================ */
UdpServer* UdpServerConstruct(uv_loop_t *loop) {
    UdpServer *server = (UdpServer*)calloc(1, sizeof(UdpServer));
    if (!server) return NULL;

    server->loop_        = loop;
    server->buffer_      = (char*)malloc(UDP_BUF_SIZE);
    server->buffer_size_ = UDP_BUF_SIZE;
    server->udp_         = (uv_udp_t*)malloc(sizeof(uv_udp_t));

    if (!server->buffer_ || !server->udp_) {
        free(server->buffer_);
        free(server->udp_);
        free(server);
        return NULL;
    }

    uv_udp_init(loop, server->udp_);
    server->udp_->data = server;

    return server;
}

/* ============================================
 * 公有 — 析构 (异步，资源在 on_close_cb 中释放)
 * ============================================ */
void UdpServerDestruct(UdpServer* server) {
    if (!server) return;
    if (server->udp_ && !uv_is_closing((uv_handle_t*)server->udp_)) {
        uv_close((uv_handle_t*)server->udp_, on_close_cb);
    } else {
        free(server->buffer_);
        free(server->udp_);
        free(server);
    }
}

/* ============================================
 * 公有 — 设置回调
 * ============================================ */
void UdpServerSetOnRecv(UdpServer* server, UdpServerOnRecv cb) {
    server->on_recv_ = cb;
}

void UdpServerSetOnSend(UdpServer* server, UdpServerOnSend cb) {
    server->on_send_ = cb;
}

void UdpServerSetOnError(UdpServer* server, UdpServerOnError cb) {
    server->on_error_ = cb;
}

void UdpServerSetOnClose(UdpServer* server, UdpServerOnClose cb) {
    server->on_close_ = cb;
}

/* ============================================
 * 公有 — 绑定地址
 * ============================================ */
int UdpServerBind(UdpServer* server, const char* ip, int port) {
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

    return uv_udp_bind(server->udp_, (const struct sockaddr*)&addr, 0);
}

/* ============================================
 * 公有 — 启动接收
 * ============================================ */
int UdpServerStartRecv(UdpServer* server) {
    return uv_udp_recv_start(server->udp_, on_alloc, on_recv_cb);
}

/* ============================================
 * 公有 — 发送数据
 * ============================================ */
int UdpServerSend(UdpServer* server, const char* data, size_t length,
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

    ctx->server    = server;
    ctx->data_len  = length;
    memcpy(ctx->data, data, length);

    uv_buf_t buf = uv_buf_init(ctx->data, (unsigned int)length);

    ret = uv_udp_send(&ctx->req, server->udp_, &buf, 1,
                      (const struct sockaddr*)&addr, on_send_done);
    if (ret < 0) {
        free(ctx);
    }
    return ret;
}
