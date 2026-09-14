#include "http3_frame.h"
#include "http3_common.h"
#include "quic_connection.h"
#include "quic_crypto.h"
#include "quic_packet.h"
#include "logger.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uv.h>

/* ── globals ─────────────────────────────────── */
static QuicConnection *g_conn = NULL;
static int      g_quit           = 0;
static int      g_session_ok     = 0;
static uint64_t g_connect_sid    = 0; /* CONNECT request stream */
static uint64_t g_echo_sid       = 0; /* echo data stream */
static int      g_echo_sid_valid = 0;
static int      g_seq            = 0;

static uint8_t  g_rx_buf[65536];
static size_t   g_rx_len  = 0;

/* ── helpers ──────────────────────────────────── */
static void send_wt_connect(void);
static void send_echo(int seq);

/* ── signal ───────────────────────────────────── */
static void on_signal(uv_signal_t *sig, int signum) {
    LOG_WARN("[wt-echo-cli] >>>SIGNAL %d", signum);
    g_quit = 1;
    uv_stop(sig->loop);
}

/* ================================================
 * QPACK simple encode — literal field line
 * ================================================ */
static size_t qpack_write_literal(uint8_t *buf, size_t cap,
                                   const char *name, const char *val) {
    size_t nlen = strlen(name), vlen = strlen(val), pos = 0;
    if (nlen < 7) /* < 7: fits in 3-bit prefix; else prefix=7 + continuation */
        buf[pos++] = (uint8_t)(0x20 | nlen);
    else {
        buf[pos++] = 0x27; uint64_t r = (uint64_t)nlen - 7;
        while (r >= 128) { buf[pos++] = (uint8_t)((r & 0x7f) | 0x80); r >>= 7; }
        buf[pos++] = (uint8_t)(r & 0x7f);
    }
    memcpy(buf + pos, name, nlen); pos += nlen;
    if (vlen < 128)
        buf[pos++] = (uint8_t)(vlen & 0x7f);
    else {
        buf[pos++] = (uint8_t)(((unsigned)vlen & 0x7f) | 0x80);
        uint64_t r = (uint64_t)(vlen >> 7);
        while (r >= 128) { buf[pos++] = (uint8_t)((r & 0x7f) | 0x80); r >>= 7; }
        buf[pos++] = (uint8_t)(r & 0x7f);
    }
    memcpy(buf + pos, val, vlen); pos += vlen;
    return pos;
}

/* ================================================
 * Stream callback
 * ================================================ */
static void on_stream_data(QuicConnection *conn, uint64_t stream_id,
                            const uint8_t *data, size_t len, int fin) {
    (void)fin; (void)conn;

    LOG_INFO("[wt-echo-cli] <<< RECV STREAM_DATA (stream %llu), len:%zu, fin:%d", 
        (unsigned long long)stream_id, len, fin);
    if (data && len > 0) {
        LOG_INFO("[wt-echo-cli] <<< RECV STREAM DATA: %.*s", (int)(len < 80 ? len : 80), data);
    }
    /* uni stream: H3 control (both client %4==2 and server %4==3) */
    if (stream_id % 4 != 0) {
        if (len < 1) return;
        uint8_t stype = data[0]; data++; len--;
        if (stype == H3_STREAM_TYPE_CONTROL && len > 0) {
            uint64_t ids[8], vals[8]; int count;
            if (h3_frame_parse_settings(data, len, ids, vals, 8, &count) > 0) {
                LOG_INFO("[wt-echo-cli] server SETTINGS: %d params", count);
                if (!g_connect_sid) send_wt_connect();
            }
        }
        return;
    }

    /* bidi stream — before session: CONNECT 200 response */
    if (!g_session_ok) {
        LOG_WARN("[wt-echo-cli] >>>CONNECT_OK (stream %llu): %.*s",
                 (unsigned long long)stream_id, (int)(len < 120?len:120), data);
        g_session_ok = 1;

        /* open a NEW bidi stream for echo — CONNECT stream can't be reused */
        g_echo_sid = QuicConnectionStreamOpen(g_conn);
        if (g_echo_sid == UINT64_MAX) {
            LOG_ERROR("[wt-echo-cli] open echo stream failed");
            g_quit = 1; uv_stop(uv_default_loop()); return;
        }
        g_echo_sid_valid = 1;
        LOG_WARN("[wt-echo-cli] >>>ECHO_STREAM sid=%llu", (unsigned long long)g_echo_sid);
        send_echo(1);
        return;
    }

    /* WebTransport already strips frame headers at the protocol level.
     * data received here is raw echo payload — append as-is to buffer. */
    if (g_rx_len + len > sizeof(g_rx_buf)) { g_rx_len = 0; }
    memcpy(g_rx_buf + g_rx_len, data, len); g_rx_len += len;

    while (1) {
        size_t start;
        for (start = 0; start < g_rx_len; start++)
            if (g_rx_buf[start] == '{') break;
        if (start == g_rx_len) { g_rx_len = 0; break; }
        if (start > 0) {
            memmove(g_rx_buf, g_rx_buf + start, g_rx_len - start);
            g_rx_len -= start;
        }
        int level = 0; size_t end;
        for (end = 0; end < g_rx_len; end++) {
            if (g_rx_buf[end] == '{') level++;
            if (g_rx_buf[end] == '}') { level--; if (level == 0) break; }
        }
        if (end == g_rx_len) break;
        size_t flen = end + 1;

        char expected[128];
        snprintf(expected, sizeof(expected), "{S,%d,short msg #%d}",
                 g_seq, g_seq);
        if (flen == strlen(expected) && memcmp(g_rx_buf, expected, flen) == 0) {
            LOG_WARN("[wt-echo-cli] >>>OK #%d", g_seq);
        } else {
            LOG_WARN("[wt-echo-cli] >>>FAIL #%d: got %.*s expected %s",
                     g_seq, (int)(flen < 60?flen:60), g_rx_buf, expected);
        }
        send_echo(g_seq + 1);
        g_rx_len -= flen;
        if (g_rx_len > 0)
            memmove(g_rx_buf, g_rx_buf + flen, g_rx_len);
    }
}

/* ── connected ────────────────────────────────── */
static void on_connected(QuicConnection *conn) {
    LOG_INFO("[wt-echo-cli] handshake done");
    QuicConnectionSetOnStreamData(conn, on_stream_data);

    uint64_t ctrl_sid = QuicConnectionStreamOpenUni(conn);
    if (ctrl_sid == UINT64_MAX) {
        LOG_ERROR("[wt-echo-cli] open control stream failed"); return;
    }
    uint8_t sbuf[280]; sbuf[0] = H3_STREAM_TYPE_CONTROL;
    uint64_t ids[]  = { H3_SETTING_QPACK_MAX_TABLE_CAPACITY,
                         H3_SETTING_MAX_FIELD_SECTION_SIZE,
                         H3_SETTING_ENABLE_CONNECT_PROTOCOL,
                         H3_SETTING_ENABLE_WEBTRANSPORT_DRAFT02,
                         H3_SETTING_ENABLE_WEBTRANSPORT,
                         H3_SETTING_H3_DATAGRAM };
    uint64_t vals[] = { 4096, 16384, 1, 1, 1, 1 };
    int flen = h3_frame_write_settings(sbuf + 1, sizeof(sbuf) - 1, ids, vals, 6);
    QuicConnectionStreamSend(conn, ctrl_sid, sbuf, (size_t)(1 + flen), 0);
    LOG_INFO("[wt-echo-cli] client SETTINGS sent");
}

/* ── send WebTransport CONNECT ─────────────────── */
static void send_wt_connect(void) {
    uint8_t hdr[512]; size_t pos = 0;
    hdr[pos++] = 0x00; hdr[pos++] = 0x00;
    pos += qpack_write_literal(hdr + pos, sizeof(hdr)-pos, ":method", "CONNECT");
    pos += qpack_write_literal(hdr + pos, sizeof(hdr)-pos, ":protocol", "webtransport");
    pos += qpack_write_literal(hdr + pos, sizeof(hdr)-pos, ":scheme", "https");
    pos += qpack_write_literal(hdr + pos, sizeof(hdr)-pos, ":path", "/");
    pos += qpack_write_literal(hdr + pos, sizeof(hdr)-pos, ":authority", "127.0.0.1:4433");

    uint8_t frame[1024];
    int flen = h3_frame_write_headers(frame, sizeof(frame), hdr, pos);
    if (flen < 0) { LOG_ERROR("[wt-echo-cli] HEADERS frame failed"); return; }

    g_connect_sid = QuicConnectionStreamOpen(g_conn);
    if (g_connect_sid == UINT64_MAX) {
        LOG_ERROR("[wt-echo-cli] open CONNECT bidi stream failed"); return;
    }
    LOG_WARN("[wt-echo-cli] >>>CONNECT_SEND sid=%llu", (unsigned long long)g_connect_sid);
    QuicConnectionStreamSend(g_conn, g_connect_sid, frame, (size_t)flen, 0);
}

/* ── send echo ─────────────────────────────────── */
static void send_echo(int seq) {
    if (g_quit || !g_session_ok || !g_echo_sid_valid) return;
    g_seq = seq;
    char msg[128];
    int mlen = snprintf(msg, sizeof(msg), "{S,%d,short msg #%d}", seq, seq);
    LOG_WARN("[wt-echo-cli] >>>SEND #%d: %.*s", seq, mlen, msg);
    /* prepend WT STREAM frame header: frame_type(0x41, 2B varint) + session_id(0, 1B varint)
     * 不加此头，服务端 quic_varint_read 会把 '{' 误当成帧头吃掉 */
    uint8_t framed[256];
    framed[0] = 0x40; framed[1] = 0x41; framed[2] = 0x00;
    memcpy(framed + 3, msg, (size_t)mlen);
    QuicConnectionStreamSend(g_conn, g_echo_sid, framed, (size_t)(mlen + 3), 0);
}

/* ================================================
 * Main
 * ================================================ */
int main(int argc, char *argv[]) {
    const char *ip   = "127.0.0.1";
    int         port = 4433;
    int         interval_ms = 100;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--ip") == 0 && i + 1 < argc) ip = argv[++i];
        else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) port = atoi(argv[++i]);
        else if (strcmp(argv[i], "--interval") == 0 && i + 1 < argc) interval_ms = atoi(argv[++i]);
    }
    setbuf(stdout, NULL);
    log_set_level(INFO);
    log_set_console(0);
    log_set_file("/tmp/http3_wt_echo_client.log");
    uv_loop_t *loop = uv_default_loop();

    uv_signal_t sig;
    uv_signal_init(loop, &sig);
    uv_signal_start(&sig, on_signal, SIGINT);

    g_conn = QuicConnectionCreate(loop);
    if (!g_conn) { LOG_ERROR("[wt-echo-cli] create conn failed"); return 1; }
    QuicConnectionSetOnConnected(g_conn, on_connected);
    if (QuicConnectionConnect(g_conn, ip, port) < 0) {
        LOG_ERROR("[wt-echo-cli] connect failed"); return 1;
    }
    LOG_WARN("[wt-echo-cli] >>>CONNECTING to %s:%d", ip, port);

    uv_run(loop, UV_RUN_DEFAULT);

    if (g_conn) { QuicConnectionDestruct(g_conn); g_conn = NULL; }
    quic_crypto_cleanup();
    log_shutdown();
    return 0;
}
