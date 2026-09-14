#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uv.h>
#include "udp_client.h"
#include "logger.h"

static UdpClient *g_client  = NULL;
static uv_timer_t g_timer;
static int        g_seq    = 0;
static int        g_closing = 0;

/* ============================================
 * timer — 每 2 秒发送一次
 * ============================================ */
static void on_timer(uv_timer_t *timer) {
    if (g_closing) return;
    char msg[256];
    g_seq++;
    snprintf(msg, sizeof(msg), "ping #%d", g_seq);
    int ret = UdpClientSend(g_client, msg, strlen(msg), "127.0.0.1", 12345);
    if (ret < 0) {
        LOG_ERROR("[client] send failed: %s", uv_strerror(ret));
    } else {
        LOG_INFO("[client] sent '%s'", msg);
    }
}

static void client_on_send(UdpClient *client, int status) {
    if (status < 0) {
        LOG_ERROR("[client] send error: %s", uv_strerror(status));
    }
}

static void client_on_recv(UdpClient *client, const char *data, ssize_t nread,
                           const char *from_ip, int from_port) {
    LOG_INFO("[client] echo recv %zd bytes from %s:%d — %.*s",
           nread, from_ip, from_port, (int)nread, data);
}

static void client_on_close(UdpClient *client) {
    LOG_INFO("[client] closed");
    uv_timer_stop(&g_timer);
    uv_close((uv_handle_t*)&g_timer, NULL);
}

static void on_signal(uv_signal_t *sig, int signum) {
    LOG_INFO("[client] signal %d, shutting down", signum);
    g_closing = 1;
    uv_signal_stop(sig);
    uv_close((uv_handle_t*)sig, NULL);
    UdpClientDestruct(g_client);
    g_client = NULL;
}

int main(int argc, char *argv[]) {
    uv_loop_t *loop = uv_default_loop();

    /* 信号处理 */
    uv_signal_t sig;
    uv_signal_init(loop, &sig);
    uv_signal_start(&sig, on_signal, SIGINT);

    /* udp client */
    g_client = UdpClientConstruct(loop);
    UdpClientSetOnSend(g_client, client_on_send);
    UdpClientSetOnRecv(g_client, client_on_recv);
    UdpClientSetOnClose(g_client, client_on_close);

    int ret = UdpClientBind(g_client, "127.0.0.1", 0);
    if (ret < 0) {
        LOG_ERROR("bind failed: %s", uv_strerror(ret));
        return 1;
    }

    ret = UdpClientStartRecv(g_client);
    if (ret < 0) {
        LOG_ERROR("start recv failed: %s", uv_strerror(ret));
        return 1;
    }

    /* 定时器 — 每 2 秒发一次 */
    uv_timer_init(loop, &g_timer);
    uv_timer_start(&g_timer, on_timer, 0, 2000);

    LOG_INFO("[client] started, sending 'ping #N' every 2s to 127.0.0.1:12345");
    LOG_INFO("[client] press Ctrl+C to stop");

    uv_run(loop, UV_RUN_DEFAULT);
    uv_loop_close(loop);
    log_shutdown();
    return 0;
}
