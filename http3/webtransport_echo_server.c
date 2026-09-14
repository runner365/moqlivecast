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

static void on_stream_data(wt_server_t *srv, wt_session_t *sess,
                            wt_stream_t *st,
                            const uint8_t *data, size_t len, void *user) {
    (void)srv; (void)sess; (void)user;
    LOG_INFO("[wt-echo-srv] echo %zu bytes", len);
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
