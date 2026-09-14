#include "quic_client_api.h"
#include "logger.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uv.h>

/* Callback-pacing echo client using quic_client_api.
 * on_connect → open_stream → on_stream_open → write first msg
 * → write_cb(ret=0) → write next → ...
 * ret<0: LOG_ERROR + close + exit.
 * on_stream_data: print received echo. */

static quic_client_t *g_c      = NULL;
static quic_stream_t *g_stream = NULL;
static int  g_seq     = 0;
static int  g_quit    = 0;
static char g_server_ip[64]    = "127.0.0.1";
static int  g_server_port      = 4433;
static int  g_count            = 20;

/* ── write callback: ret=0 → next; ret<0 → fail ── */
static void on_write_done(quic_stream_t *s, int ret, void *user) {
    (void)user;
    if (ret != 0) {
        LOG_ERROR("[echo] WRITE FAIL ret=%d → exit", ret);
        g_quit = 1; quic_client_close(g_c);
        return;
    }
    if (g_seq >= g_count) { g_quit = 1; quic_client_close(g_c); return; }
    if (g_quit) return;
    g_seq++;
    char msg[128];
    int n = snprintf(msg, sizeof(msg), "Hello QUIC! #%d", g_seq);
    LOG_WARN("[echo] write #%d", g_seq);
    quic_client_stream_write(s, (const uint8_t*)msg, (size_t)n, 0,
                              on_write_done, NULL, 0);
}

/* ── stream data callback ───────────────────────── */
static void on_stream_data(quic_client_t *c, quic_stream_t *s,
                            const uint8_t *data, size_t len, void *user) {
    (void)c; (void)s; (void)user;
    LOG_INFO("[echo] recv %zu bytes: %.*s", len, (int)len, data);
}

/* ── connect → open stream ─────────────────────── */
static void on_connect(quic_client_t *c, int status, void *user) {
    (void)user;
    if (status != 0) {
        LOG_ERROR("[echo] connect fail: %d", status);
        g_quit = 1; return;
    }
    LOG_WARN("[echo] CONNECTED → open stream");
    quic_client_open_stream(c);
}

/* ── stream open → kickoff first write ─────────── */
static void on_stream_open(quic_client_t *c, quic_stream_t *s, void *user) {
    (void)user;
    g_stream = s;
    LOG_WARN("[echo] STREAM_OPEN → start echo (%d msgs)", g_count);
    /* first write triggers the callback chain */
    on_write_done(s, 0, NULL);
}

/* ── close ──────────────────────────────────────── */
static void on_close(quic_client_t *c, int err, void *user) {
    (void)c; (void)user;
    LOG_WARN("[echo] CLOSED err=%d → stop loop", err);
    uv_stop(uv_default_loop());
}

static void on_signal(uv_signal_t *sig, int signum) {
    LOG_WARN("[echo] SIG %d → quit", signum);
    g_quit = 1;
    if (g_c) quic_client_close(g_c);
    uv_signal_stop(sig); uv_close((uv_handle_t*)sig, NULL);
}

/* ── main ──────────────────────────────────────── */
int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--ip") && i+1 < argc)
            snprintf(g_server_ip, sizeof(g_server_ip), "%s", argv[++i]);
        else if (!strcmp(argv[i], "--port") && i+1 < argc)
            g_server_port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--count") && i+1 < argc)
            g_count = atoi(argv[++i]);
    }
    setbuf(stdout, NULL);
    log_set_level(INFO); log_set_console(0);
    log_set_file("/tmp/quic_echo_client.log");

    uv_loop_t *lp = uv_default_loop();
    uv_signal_t sig; uv_signal_init(lp, &sig);
    uv_signal_start(&sig, on_signal, SIGINT);

    quic_client_callbacks_t cb = {0};
    cb.on_connect     = on_connect;
    cb.on_stream_open = on_stream_open;
    cb.on_stream_data = on_stream_data;
    cb.on_close       = on_close;

    g_c = quic_client_new(lp, &cb);
    if (!g_c) { LOG_ERROR("[echo] new fail"); return 1; }
    quic_client_connect(g_c, g_server_ip, g_server_port);

    uv_run(lp, UV_RUN_DEFAULT);
    uv_loop_close(lp);
    g_c = NULL;
    return 0;
}
