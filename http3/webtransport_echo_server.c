#include "webtransport_server_api.h"
#include "logger.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uv.h>

static wt_server_t *g_srv = NULL;

static void on_signal(uv_signal_t *sig, int signum) {
    LOG_WARN("[wt-echo-srv] >>>SIGNAL %d", signum);
    if (g_srv) wt_server_destroy(g_srv);
    g_srv = NULL;
    uv_stop(sig->loop);
}

static void on_session(wt_server_t *srv, wt_session_t *sess, void *user) {
    (void)srv; (void)user;
    const char *path = wt_session_get_path(sess);
    LOG_WARN("[wt-echo-srv] >>>SESSION path=%s", path ? path : "/");
}

/* 回送用的单向流出流：每个会话一条，惰性创建后挂在 session 上。
 * 收到客户端单向流数据时，在这条【服务端发起的】流上把原样内容送回 ——
 * 单向流不能在对端发起的流上回写（会触发 PROTOCOL_VIOLATION），
 * 所以 echo 的「回」只能靠新开一条单向流完成。 */
static wt_stream_t *echo_stream_of(wt_session_t *sess) {
    wt_stream_t *out = (wt_stream_t*)wt_session_get_user_data(sess);
    if (out) return out;

    out = wt_server_open_uni_stream(sess);
    if (!out) {
        /* 通常是客户端还没给出 MAX_STREAMS_UNI 配额 —— 流控的正常情形。
         * 本会话首包可能早于配额到达，此处放弃本条回送，后续再试。 */
        LOG_WARN("[wt-echo-srv] open echo uni stream failed (no credit yet)");
        return NULL;
    }
    wt_session_set_user_data(sess, out);
    LOG_WARN("[wt-echo-srv] echo uni stream ready");
    return out;
}

static uint64_t g_uni_recv_bytes = 0;
static int      g_uni_recv_count = 0;

static void on_stream_data(wt_server_t *srv, wt_session_t *sess,
                            wt_stream_t *st,
                            const uint8_t *data, size_t len, void *user) {
    (void)srv; (void)user;

    if (wt_stream_is_uni(st)) {
        g_uni_recv_bytes += len;
        g_uni_recv_count++;

        char preview[160];
        size_t n = len < sizeof(preview) - 1 ? len : sizeof(preview) - 1;
        memcpy(preview, data, n);
        preview[n] = '\0';
        LOG_WARN("[wt-echo-srv] recv %zu bytes on UNI stream "
                 "(total %zu bytes in %d frames) payload=\"%s\"",
                 len, (size_t)g_uni_recv_bytes, g_uni_recv_count, preview);

        /* echo：原样回送到本会话的回流上。
         * 不能在本流回写，故走服务端自建的单向流。 */
        wt_stream_t *out = echo_stream_of(sess);
        if (out) {
            int r = wt_stream_write(out, data, len);
            LOG_WARN("[wt-echo-srv] echo back %zu bytes on uni stream, r=%d",
                     len, r);
        }
        return;
    }

    char preview[160];
    size_t n = len < sizeof(preview) - 1 ? len : sizeof(preview) - 1;
    memcpy(preview, data, n);
    preview[n] = '\0';
    LOG_INFO("[wt-echo-srv] echo %zu bytes payload=\"%s\"", len, preview);
    wt_stream_write(st, data, len);
}

int main(int argc, char *argv[]) {
    const char *cert_file = NULL, *key_file = NULL, *ip = NULL;
    int port = 4433;
    for (int i = 1; i < argc; i++) {
        if (!cert_file)      cert_file = argv[i];
        else if (!key_file)  key_file  = argv[i];
        else if (!ip)        ip        = argv[i];
        else                 port      = atoi(argv[i]);
    }
    if (!cert_file) cert_file = "cert/server_cert.pem";
    if (!key_file)  key_file  = "cert/server_key.pem";
    if (!ip)        ip        = "127.0.0.1";

    setbuf(stdout, NULL);
    log_set_level(INFO);
    log_set_console(0);
    log_set_file("/tmp/wt_echo_server.log");

    uv_loop_t *loop = uv_default_loop();
    uv_signal_t sig; uv_signal_init(loop, &sig);
    uv_signal_start(&sig, on_signal, SIGINT);

    g_srv = wt_server_create(loop);
    if (!g_srv) { LOG_ERROR("[wt-echo-srv] create failed"); return 1; }

    wt_path_callbacks_t cb = {0};
    cb.on_session     = on_session;
    cb.on_stream_data = on_stream_data;
    wt_server_add_path(g_srv, "/", &cb);

    if (wt_server_listen(g_srv, cert_file, key_file, ip, port) < 0) {
        LOG_ERROR("[wt-echo-srv] listen failed"); return 1;
    }
    LOG_WARN("[wt-echo-srv] >>>LISTENING on %s:%d", ip, port);

    uv_run(loop, UV_RUN_DEFAULT);
    g_srv = NULL;
    log_shutdown();
    return 0;
}
