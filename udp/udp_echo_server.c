#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uv.h>
#include "udp_server.h"
#include "logger.h"

static UdpServer *g_server  = NULL;
static int        g_closing = 0;

static void server_on_recv(UdpServer *server, const char *data, ssize_t nread,
                           const char *from_ip, int from_port) {
    LOG_INFO("[server] recv %zd bytes from %s:%d — %.*s",
           nread, from_ip, from_port, (int)nread, data);

    int ret = UdpServerSend(server, data, (size_t)nread, from_ip, from_port);
    if (ret < 0) {
        LOG_ERROR("[server] echo failed: %s", uv_strerror(ret));
    } else {
        LOG_INFO("[server] echoed");
    }
}

static void server_on_send(UdpServer *server, int status) {
    if (status < 0) {
        LOG_ERROR("[server] send error: %s", uv_strerror(status));
    }
}

static void server_on_error(UdpServer *server, int errcode) {
    LOG_ERROR("[server] recv error: %s", uv_strerror(errcode));
}

static void server_on_close(UdpServer *server) {
    LOG_INFO("[server] closed");
}

static void on_signal(uv_signal_t *sig, int signum) {
    LOG_INFO("[server] signal %d, shutting down", signum);
    g_closing = 1;
    uv_signal_stop(sig);
    uv_close((uv_handle_t*)sig, NULL);
    UdpServerDestruct(g_server);
    g_server = NULL;
    uv_stop(sig->loop);
}

int main(int argc, char *argv[]) {
    uv_loop_t *loop = uv_default_loop();

    /* 信号处理 */
    uv_signal_t sig;
    uv_signal_init(loop, &sig);
    uv_signal_start(&sig, on_signal, SIGINT);

    /* udp server */
    g_server = UdpServerConstruct(loop);
    UdpServerSetOnRecv(g_server, server_on_recv);
    UdpServerSetOnSend(g_server, server_on_send);
    UdpServerSetOnError(g_server, server_on_error);
    UdpServerSetOnClose(g_server, server_on_close);

    int ret = UdpServerBind(g_server, "127.0.0.1", 12345);
    if (ret < 0) {
        LOG_ERROR("bind failed: %s", uv_strerror(ret));
        return 1;
    }

    ret = UdpServerStartRecv(g_server);
    if (ret < 0) {
        LOG_ERROR("start recv failed: %s", uv_strerror(ret));
        return 1;
    }

    LOG_INFO("[server] listening on 127.0.0.1:12345");
    LOG_INFO("[server] press Ctrl+C to stop");

    uv_run(loop, UV_RUN_DEFAULT);

    /* 处理 close 回调 */
    uv_run(loop, UV_RUN_DEFAULT);

    uv_loop_close(loop);
    log_shutdown();
    return 0;
}
