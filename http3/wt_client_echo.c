#include "webtransport_client_api.h"
#include "logger.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uv.h>

/* ── Callback-pacing echo client ──────────────
 * Send one message, wait for ACK callback,
 * then send the next.  No thread, no sleep. */

static wt_client_t *g_cli   = NULL;
static wt_stream_t *g_stream = NULL;
static int  g_seq    = 0;
static int  g_quit   = 0;
static char g_server_ip[64]   = "127.0.0.1";
static int  g_server_port     = 4433;
static int  g_echo_count      = 0;
static int  g_echo_max        = 20;

static const char *s_content =
    "Meta shares fall as frustration grows over AI spending plans";

static char s_msg[1400];
static char s_recv[1400];
static int  s_rlen = 0;

/* ── signal ──────────────────────────────── */
static void on_signal(uv_signal_t *sig, int signum) {
    LOG_WARN("[wt-echo] SIG %d", signum);
    g_quit = 1;
    if (g_cli) wt_client_close(g_cli);
    uv_signal_stop(sig);
    uv_close((uv_handle_t*)sig, NULL);
}

/* ── on_close ─────────────────────────────── */
static void on_wt_close(wt_client_t *cli, int err, void *user) {
    (void)cli; (void)err; (void)user;
    LOG_WARN("[wt-echo] CLOSED err=%d → stop", err);
    uv_stop(uv_default_loop());
}

/* ── write callback: send next message ─────── */
static void on_write_done(wt_stream_t *s, int ret, void *user) {
    (void)user;
    if (ret != 0) {
        LOG_ERROR("[wt-echo] write failed: ret=%d", ret);
        g_quit = 1; wt_client_close(g_cli); return;
    }
    if (g_echo_count >= g_echo_max) { g_quit = 1; wt_client_close(g_cli); return; }
    if (g_quit) return;

    g_seq++;
    int mlen = snprintf(s_msg, sizeof(s_msg), "{S,%d,message:%s #%d}",
                        g_seq, s_content, g_seq);
    g_echo_count++;
    LOG_WARN("[wt-echo] SEND #%d (%d bytes, wrote=%d/%d)",
             g_seq, mlen, g_echo_count, g_echo_max);
    wt_stream_write_cb(s, (const uint8_t*)s_msg, (size_t)mlen,
                        on_write_done, NULL, 5000);
}

/* ── stream data: validate echo ────────────── */
static void on_stream_data(wt_client_t *cli, wt_stream_t *st,
                            const uint8_t *data, size_t len, void *user) {
    (void)user; (void)cli; (void)st;
    if (s_rlen + len < sizeof(s_recv)) {
        memcpy(s_recv + s_rlen, data, len);
        s_rlen += len;
    }
    /* frame-aware parse */
    while (1) {
        size_t start;
        for (start = 0; start < (size_t)s_rlen; start++)
            if (s_recv[start] == '{') break;
        if (start == (size_t)s_rlen) { s_rlen = 0; break; }
        if (start > 0) { memmove(s_recv, s_recv + start, s_rlen - start); s_rlen -= start; }
        int level = 0; size_t end;
        for (end = 0; end < (size_t)s_rlen; end++) {
            if (s_recv[end] == '{') level++;
            if (s_recv[end] == '}') { level--; if (level == 0) break; }
        }
        if (end == (size_t)s_rlen) break;
        size_t flen = end + 1;
        int rx_seq = 0;
        sscanf(s_recv, "{S,%d,", &rx_seq);
        char expected[1400];
        int elen = snprintf(expected, sizeof(expected),
                            "{S,%d,message:%s #%d}", rx_seq, s_content, rx_seq);
        if (flen == (size_t)elen && memcmp(s_recv, expected, (size_t)elen) == 0)
            LOG_WARN("[wt-echo] OK #%d (%zu bytes)", rx_seq, flen);
        else
            LOG_WARN("[wt-echo] FAIL: got %.*s expected %.*s",
                     (int)(flen < 60 ? flen : 60), s_recv,
                     (int)(elen < 60 ? elen : 60), expected);
        s_rlen -= flen;
        if (s_rlen > 0) memmove(s_recv, s_recv + flen, s_rlen);
    }
}

/* ── on_connect → open stream ─────────────── */
static void on_connect(wt_client_t *cli, int status, void *user) {
    (void)user; (void)cli;
    if (status != 0) { LOG_ERROR("[wt-echo] connect fail"); return; }
    LOG_WARN("[wt-echo] CONNECTED");
    wt_client_open_stream(cli);
}

/* ── on_stream_open → kickoff first write ──── */
static void on_stream_open(wt_client_t *cli, wt_stream_t *st, void *user) {
    (void)user; (void)cli;
    g_stream = st;
    LOG_WARN("[wt-echo] STREAM_OPEN — starting echo");
    /* first write triggers the callback chain */
    on_write_done(st, 0, NULL);
}

/* ── main ─────────────────────────────────── */
int main(int argc, char *argv[]) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--ip") == 0 && i + 1 < argc)
            snprintf(g_server_ip, sizeof(g_server_ip), "%s", argv[++i]);
        else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc)
            g_server_port = atoi(argv[++i]);
        else if (strcmp(argv[i], "--count") == 0 && i + 1 < argc)
            g_echo_max = atoi(argv[++i]);
    }
    setbuf(stdout, NULL);
    log_set_level(INFO);
    log_set_console(0);
    log_set_file("/tmp/wt_client_echo.log");

    uv_loop_t *loop = uv_default_loop();
    uv_signal_t sig; uv_signal_init(loop, &sig);
    uv_signal_start(&sig, on_signal, SIGINT);

    wt_callbacks_t cb = {0};
    cb.on_connect     = on_connect;
    cb.on_stream_open = on_stream_open;
    cb.on_stream_data = on_stream_data;
    cb.on_close       = on_wt_close;

    g_cli = wt_client_new(loop, &cb);
    if (!g_cli) { LOG_ERROR("[wt-echo] new fail"); return 1; }
    wt_client_connect(g_cli, g_server_ip, g_server_port);
    uv_run(loop, UV_RUN_DEFAULT);
    g_cli = NULL;
    return 0;
}
