/* wt2ws_smoke — end-to-end smoke test for wt2ws_proxy.
 *
 *   wt2ws_smoke <host> <port> [path]
 *
 * Covers what matters for a relay:
 *   - two frames coalesced in one WT write (message boundaries preserved)
 *   - one 200 KB frame split across many QUIC packets (partial parse)
 *   - one 1 KB binary frame (text/binary bit preserved)
 *
 * Exit 0 = pass, 1 = fail/timeout.
 */
#include "webtransport_client_api.h"
#include "logger.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uv.h>

#define BIG_LEN 200000
#define BIN_LEN 1024

static wt_client_t *g_cli = NULL;
static wt_stream_t *g_stream = NULL;
static uv_timer_t g_timeout;
static int g_failed = 0;

static uint8_t g_big[BIG_LEN];
static uint8_t g_bin[BIN_LEN];

static const char *kOne = "one";
static const char *kTwo = "two";

struct Expect {
    uint8_t type;
    const uint8_t *data;
    size_t len;
};
static struct Expect g_expect[4];
static int g_expect_count = 0;
static int g_expect_next = 0;

static uint8_t g_rx[256 * 1024];
static size_t g_rx_used = 0;

static void fail(const char *why) {
    if (g_failed) return;
    g_failed = 1;
    fprintf(stderr, "[smoke] FAIL: %s\n", why);
    if (g_cli) wt_client_close(g_cli);
    uv_stop(uv_default_loop());
}

static void pass(void) {
    printf("[smoke] PASS: coalesced + 200KB + binary relayed through proxy\n");
    if (g_cli) wt_client_close(g_cli);
    uv_stop(uv_default_loop());
}

static size_t frame(uint8_t type, const uint8_t *data, size_t len, uint8_t *out) {
    out[0] = (uint8_t)((len >> 24) & 0xff);
    out[1] = (uint8_t)((len >> 16) & 0xff);
    out[2] = (uint8_t)((len >> 8) & 0xff);
    out[3] = (uint8_t)(len & 0xff);
    out[4] = type;
    memcpy(out + 5, data, len);
    return len + 5;
}

static void send_frame(uint8_t type, const uint8_t *data, size_t len) {
    uint8_t *buf = (uint8_t *)malloc(len + 5);
    if (!buf) { fail("oom"); return; }
    size_t n = frame(type, data, len, buf);
    wt_stream_write_cb(g_stream, buf, n, NULL, NULL, 5000);
    free(buf); /* wt_stream_write_cb copies */
}

static void send_all(void) {
    /* two messages in one write: the proxy must keep them separate */
    size_t n1 = 5 + strlen(kOne), n2 = 5 + strlen(kTwo);
    uint8_t *batch = (uint8_t *)malloc(n1 + n2);
    if (!batch) { fail("oom"); return; }
    frame(1, (const uint8_t *)kOne, strlen(kOne), batch);
    frame(1, (const uint8_t *)kTwo, strlen(kTwo), batch + n1);
    wt_stream_write_cb(g_stream, batch, n1 + n2, NULL, NULL, 5000);
    free(batch);

    /* 200 KB text frame: arrives in many WT chunks */
    send_frame(1, g_big, BIG_LEN);

    /* binary frame */
    send_frame(2, g_bin, BIN_LEN);
}

static void on_connect(wt_client_t *cli, int status, void *user) {
    (void)cli; (void)user;
    if (status != 0) { fail("connect failed"); return; }
    printf("[smoke] connected, opening bidi stream\n");
    wt_client_open_stream(g_cli);
}

static void on_stream_open(wt_client_t *cli, wt_stream_t *s, void *user) {
    (void)cli; (void)user;
    if (wt_stream_is_uni(s)) return;
    g_stream = s;
    printf("[smoke] stream open, sending %d messages\n", g_expect_count);
    send_all();
}

static void on_stream_data(wt_client_t *cli, wt_stream_t *s,
                           const uint8_t *data, size_t len, void *user) {
    (void)cli; (void)s; (void)user;
    if (g_failed) return;
    if (g_rx_used + len > sizeof(g_rx)) { fail("echo too large"); return; }
    memcpy(g_rx + g_rx_used, data, len);
    g_rx_used += len;

    while (g_rx_used >= 5 && !g_failed) {
        const uint32_t plen = ((uint32_t)g_rx[0] << 24) |
                              ((uint32_t)g_rx[1] << 16) |
                              ((uint32_t)g_rx[2] << 8) | g_rx[3];
        if (g_rx_used < 5u + plen) break;
        if (g_expect_next >= g_expect_count) { fail("unexpected extra frame"); return; }
        {
            const struct Expect *e = &g_expect[g_expect_next];
            if (g_rx[4] != e->type || plen != e->len ||
                memcmp(g_rx + 5, e->data, plen) != 0) {
                fprintf(stderr,
                        "[smoke] mismatch frame %d: type %u len %u (want %u/%zu)\n",
                        g_expect_next, g_rx[4], plen, e->type, e->len);
                fail("payload mismatch");
                return;
            }
            printf("[smoke] echo %d/%d OK: type=%u len=%u%s\n",
                   g_expect_next + 1, g_expect_count, e->type, plen,
                   e->len > 4096 ? " (large)" : "");
        }
        g_expect_next++;
        memmove(g_rx, g_rx + 5 + plen, g_rx_used - 5 - plen);
        g_rx_used -= 5 + plen;
        if (g_expect_next == g_expect_count) { pass(); return; }
    }
}

static void on_close(wt_client_t *cli, int err, void *user) {
    (void)cli; (void)user;
    if (!g_failed && g_expect_next < g_expect_count) {
        fprintf(stderr, "[smoke] closed early err=%d (%d/%d echoes)\n",
                err, g_expect_next, g_expect_count);
        g_failed = 1;
    }
    uv_stop(uv_default_loop());
}

static void on_timeout(uv_timer_t *t) {
    (void)t;
    if (g_expect_next < g_expect_count) fail("timeout waiting for echo");
    else pass();
}

int main(int argc, char **argv) {
    const char *host = argc > 1 ? argv[1] : "127.0.0.1";
    int port = argc > 2 ? atoi(argv[2]) : 5433;
    const char *path = argc > 3 ? argv[3] : "/wt2ws";

    for (size_t i = 0; i < BIG_LEN; i++) g_big[i] = (uint8_t)('a' + (i % 26));
    for (size_t i = 0; i < BIN_LEN; i++) g_bin[i] = (uint8_t)(i & 0xff);

    g_expect[0] = (struct Expect){1, (const uint8_t *)kOne, strlen(kOne)};
    g_expect[1] = (struct Expect){1, (const uint8_t *)kTwo, strlen(kTwo)};
    g_expect[2] = (struct Expect){1, g_big, BIG_LEN};
    g_expect[3] = (struct Expect){2, g_bin, BIN_LEN};
    g_expect_count = 4;

    setbuf(stdout, NULL);
    log_set_console(0);
    log_set_file("/tmp/wt2ws_smoke.log");
    log_set_level(WARN);

    uv_loop_t *loop = uv_default_loop();
    uv_timer_init(loop, &g_timeout);
    uv_timer_start(&g_timeout, on_timeout, 15000, 0);

    wt_callbacks_t cb;
    memset(&cb, 0, sizeof(cb));
    cb.on_connect = on_connect;
    cb.on_stream_open = on_stream_open;
    cb.on_stream_data = on_stream_data;
    cb.on_close = on_close;

    g_cli = wt_client_new(loop, &cb);
    if (!g_cli) { fprintf(stderr, "wt_client_new failed\n"); return 1; }
    wt_client_connect_path(g_cli, host, port, path);
    uv_run(loop, UV_RUN_DEFAULT);
    wt_client_close(g_cli);
    return g_failed ? 1 : 0;
}
