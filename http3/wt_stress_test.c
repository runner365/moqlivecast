#include "webtransport_client_api.h"
#include "logger.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uv.h>

/* Rounds advance by write_cb (ACK). Echo validated independently in on_sd.
 * No echo_done gate — proxy reverse-channel loss doesn't block progress. */

static char  g_ip[64] = "127.0.0.1";
static int   g_port   = 4434;
static int   g_size   = 10000;
static int   g_rounds = 100;
static int   g_timeout_s = 30;
static int   g_verbose = 0;

static wt_client_t *g_cli;
static wt_stream_t *g_stream;
static int g_ok;
static int g_stop;

static uint8_t *g_send, *g_recv;
static size_t   g_roff;
static int      g_rnd;         /* current round (1-based) */
static int      g_werr, g_terr, g_wcnt;  /* write errors / total errors / write count */
static uint64_t g_tstart, g_rstart;
static int      g_done;

static void start_round(void);
static void write_done_cb(wt_stream_t *s, int ret, void *user);
static void advance_round(void);

static void fill(uint8_t *b, size_t n, int r) {
    for (size_t i = 0; i < n; i++) b[i] = (uint8_t)((r * 37 + i) & 0xFF);
}
static int check(size_t off, const uint8_t *d, size_t n, int r) {
    int e = 0;
    for (size_t i = 0; i < n; i++) {
        uint8_t x = (uint8_t)((r * 37 + off + i) & 0xFF);
        if (d[i] != x) { e++; if (e <= 5) LOG_ERROR("[stress] BAD off=%zu", off + i); }
    }
    return e;
}

static void start_round(void) {
    fill(g_send, (size_t)g_size, g_rnd);
    memset(g_recv, 0, (size_t)g_size);
    g_roff = g_werr = 0;
    g_rstart = uv_now(uv_default_loop());
    if (g_verbose) LOG_INFO("[stress] >>> R%d", g_rnd);
    wt_stream_write_cb(g_stream, g_send, (size_t)g_size,
                        write_done_cb, NULL, (uint64_t)g_timeout_s * 1000);
}

/* 推进到下一轮。echo 完整（正常路径）或写超时（异常路径）时调用。 */
static void advance_round(void) {
    g_rnd++;
    if (g_rnd <= g_rounds && !g_stop) {
        start_round();
    } else if (!g_done) {
        g_done = 1;
        double tl = (uv_now(uv_default_loop()) - g_tstart) / 1000.0;
        if (!tl) tl = 1;
        uint64_t tb = (uint64_t)g_size * (uint64_t)g_rounds;
        LOG_WARN("[stress] === %s  %lluB %.1fs %.2fMbps write_err=%d echo_err=%d ===",
                 g_terr ? "FAIL" : "PASS",
                 tb, tl, (tb * 8.0 / 1e6) / tl, g_werr, g_terr);
        wt_client_close(g_cli);
    }
}

static void write_done_cb(wt_stream_t *s, int ret, void *user) {
    (void)s; (void)user;
    g_wcnt++;
    if (ret != 0) {
        LOG_ERROR("[stress] WRITE FAIL r%d ret=%d", g_rnd, ret);
        g_werr++;
        /* 写超时/失败 → echo 不会到达，推进 round 防卡死 */
        advance_round();
    }
    /* ret==0（写 ACK）→ 等 echo 完整，由 on_sd 推进。
     * 写 ACK 严格先于 echo（server 收到才回显），不会丢推进。 */
}

static void on_sd(wt_client_t *c, wt_stream_t *st,
                   const uint8_t *d, size_t n, void *u) {
    (void)c; (void)u; (void)st;
    if (g_roff + n > (size_t)g_size) n = (size_t)g_size - g_roff;
    /* 用明确的当前轮 g_rnd 校验，不靠数据首字节推断（跨轮混淆根因） */
    g_terr += check(g_roff, d, n, g_rnd);
    memcpy(g_recv + g_roff, d, n); g_roff += n;
    if (g_roff >= (size_t)g_size) {
        uint64_t el = uv_now(uv_default_loop()) - g_rstart; if (!el) el = 1;
        double mb = (g_size * 8.0 / 1e6) / (el / 1000.0);
        LOG_WARN("[stress] echo R%d OK %.0fms %.2fMbps",
                 g_rnd, (double)el, mb);
        /* echo 完整 → 推进下一轮（echo-gate：单 buffer 不跨轮混淆） */
        advance_round();
    }
}

static void on_c(wt_client_t *c, int st, void *u) {
    (void)c;(void)u; if (st) { LOG_ERROR("[stress] CXN FAIL"); return; }
    LOG_WARN("[stress] CONNECTED"); g_ok = 1; wt_client_open_stream(c);
}
static void on_so(wt_client_t *c, wt_stream_t *st, void *u) {
    (void)c;(void)u;
    if (g_stream) return;
    g_stream = st;
    LOG_WARN("[stress] STREAM_OPEN %dB x %d", g_size, g_rounds);
    g_tstart = uv_now(uv_default_loop()); g_rnd = 1; start_round();
}
static void on_cl(wt_client_t *c, int e, void *u) {
    (void)c;(void)e;(void)u; LOG_WARN("[stress] CLOSED err=%d", e); uv_stop(uv_default_loop());
}
static void on_sig(uv_signal_t *s, int n) {
    (void)n; g_stop = 1; LOG_WARN("[stress] SIG"); uv_signal_stop(s); uv_close((uv_handle_t*)s, NULL);
    if (g_cli) wt_client_close(g_cli);
}

int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--ip") && i+1 < argc) snprintf(g_ip, sizeof(g_ip), "%s", argv[++i]);
        else if (!strcmp(argv[i], "--port") && i+1 < argc) g_port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--size") && i+1 < argc) g_size = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--rounds") && i+1 < argc) g_rounds = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--timeout") && i+1 < argc) g_timeout_s = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-v") || !strcmp(argv[i], "--verbose")) g_verbose = 1;
    }
    setbuf(stdout, NULL); log_set_level(INFO); log_set_console(0);
    log_set_file("/tmp/wt_stress_test.log");
    LOG_WARN("[stress] %s:%d  %dB x %d timeout=%ds (no-echo-gate)",
             g_ip, g_port, g_size, g_rounds, g_timeout_s);
    g_send = malloc((size_t)g_size); g_recv = malloc((size_t)g_size);
    uv_loop_t *lp = uv_default_loop();
    uv_signal_t sg; uv_signal_init(lp, &sg); uv_signal_start(&sg, on_sig, SIGINT);
    wt_callbacks_t cb = {0};
    cb.on_connect = on_c; cb.on_stream_open = on_so; cb.on_stream_data = on_sd; cb.on_close = on_cl;
    g_cli = wt_client_new(lp, &cb);
    wt_client_connect(g_cli, g_ip, g_port);
    uv_run(lp, UV_RUN_DEFAULT); uv_loop_close(lp);
    free(g_send); free(g_recv);
    return g_terr ? 1 : 0;
}
