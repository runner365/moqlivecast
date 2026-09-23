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
/* --uni：改用单向流发送。echo 由服务端在自己发起的另一条单向流上回送，
 * 本端在 on_stream_data 里做与双向流相同的内容校验。 */
static int  g_use_uni         = 0;
static int  g_rx_count        = 0;   /* 已收到并校验通过的回显帧数 */

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
    if (g_echo_count >= g_echo_max) {
        /* 发完了。但还不能立刻退：
         *
         * 单向流下 echo 走的是【另一条流】（服务端自建的单向流），它与
         * 本条的 ACK 时序无关 —— ACK 可能先于 echo 到达。若此处直接
         * 关闭，echo 会被丢掉，校验落空。
         *
         * 双向流同理（echo 与 ACK 也是两条独立路径）。
         * 所以统一改为：交给 on_stream_data 收到足够回显后退出；这里
         * 只起一个兜底定时器，防止对端不回导致永久挂起。
         */
        LOG_WARN("[wt-echo] all %d frames sent, waiting for echo back...",
                 g_echo_count);
        return;
    }
    if (g_quit) return;

    g_seq++;
    int mlen = snprintf(s_msg, sizeof(s_msg), "{S,%d,message:%s #%d}",
                        g_seq, s_content, g_seq);
    g_echo_count++;
    /* 打印发送内容，与 OK/FAIL 行可直接逐字比对 */
    LOG_WARN("[wt-echo] SEND #%d (%d bytes, wrote=%d/%d): %s",
             g_seq, mlen, g_echo_count, g_echo_max, s_msg);
    wt_stream_write_cb(s, (const uint8_t*)s_msg, (size_t)mlen,
                        on_write_done, NULL, 5000);
}

/* ── 看门狗：对端不回显时的兜底退出 ─────────── */
static uv_timer_t g_wd;
static void on_watchdog(uv_timer_t *t) {
    (void)t;
    if (g_quit) return;
    LOG_WARN("[wt-echo] watchdog: %d/%d echoes received, giving up",
             g_rx_count, g_echo_max);
    g_quit = 1;
    wt_client_close(g_cli);
}

/* ── stream data: validate echo ────────────── */
/* 双向流与单向回流共用同一套校验。
 *
 * 单向流上服务端不能原地回写（会触发 PROTOCOL_VIOLATION），所以它是
 * 在【自己发起的另一条单向流】上回送的。我们这边看到的就是一条
 * 服务端发起的 uni 流，内容应与发出的完全一致 —— 因此可以复用同一
 * 校验逻辑：按 '{}' 切帧、与期望串逐字节比对。
 *
 * 只打印载荷是不够的：字节数对但内容错位（如头剥离残留）无法被发现。 */
static void on_stream_data(wt_client_t *cli, wt_stream_t *st,
                            const uint8_t *data, size_t len, void *user) {
    (void)user; (void)cli;
    const int is_uni = wt_stream_is_uni(st);

    if (s_rlen + len >= sizeof(s_recv)) return;   /* 缓冲满，丢弃本段 */
    memcpy(s_recv + s_rlen, data, len);
    s_rlen += len;

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

        /* 打印收到的原文，而不是只报长度 —— "N bytes" 无法证明内容正确
         * （头残留、错位、截断都可能长度恰好对得上）。发送侧已打印
         * SEND #N 的内容，两边可直接逐字比对。 */
        char got[256];
        int gn = (int)(flen < sizeof(got) - 1 ? flen : sizeof(got) - 1);
        memcpy(got, s_recv, (size_t)gn);
        got[gn] = '\0';

        if (flen == (size_t)elen && memcmp(s_recv, expected, (size_t)elen) == 0) {
            LOG_WARN("[wt-echo] OK #%d via %s: %s",
                     rx_seq, is_uni ? "UNI" : "BIDI", got);
        } else {
            LOG_WARN("[wt-echo] FAIL %s: got \"%s\" expected \"%s\"",
                     is_uni ? "UNI" : "BIDI", got, expected);
        }

        g_rx_count++;
        s_rlen -= flen;
        if (s_rlen > 0) memmove(s_recv, s_recv + flen, s_rlen);

        /* 收齐全部回显 → 收工。退出点放在这里而不是发送完成处：
         * echo 与 ACK 走不同路径，ACK 可能先到，此处才是"校验完成"的信号。 */
        if (g_rx_count >= g_echo_max && !g_quit) {
            LOG_WARN("[wt-echo] all %d echoes received, done", g_rx_count);
            g_quit = 1;
            wt_client_close(g_cli);
            return;
        }
    }
}

/* ── on_connect → open stream ─────────────── */
static void on_connect(wt_client_t *cli, int status, void *user) {
    (void)user;
    if (status != 0) { LOG_ERROR("[wt-echo] connect fail"); return; }
    LOG_WARN("[wt-echo] CONNECTED");
    if (g_use_uni) {
        LOG_WARN("[wt-echo] opening UNI stream");
        wt_client_open_uni_stream(cli);
    } else {
        wt_client_open_stream(cli);
    }

    /* 看门狗：发送完成后若对端始终不回显（或回显丢失），此处兜底退出，
     * 避免进程永久挂起。正常路径由 on_stream_data 收齐后退出。 */
    uv_timer_init(uv_default_loop(), &g_wd);
    uv_timer_start(&g_wd, on_watchdog, 5000, 0);
}

/* ── on_stream_open → kickoff first write ──── */
static void on_stream_open(wt_client_t *cli, wt_stream_t *st, void *user) {
    (void)user; (void)cli;
    g_stream = st;
    /* 用 API 区分流类型，而不是靠参数推断 —— 这正是新增
     * wt_stream_is_uni() 的用途，也顺带验证它返回值正确。 */
    int uni = wt_stream_is_uni(st);
    LOG_WARN("[wt-echo] STREAM_OPEN type=%s — starting echo",
             uni ? "UNI" : "BIDI");
    if (uni != g_use_uni) {
        LOG_ERROR("[wt-echo] stream type mismatch: opened=%s expected=%s",
                  uni ? "UNI" : "BIDI", g_use_uni ? "UNI" : "BIDI");
    }
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
        else if (strcmp(argv[i], "--uni") == 0)
            g_use_uni = 1;
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
