#include "http3_server_api.h"
#include "webtransport.h"
#include "quic_crypto.h"
#include "tls_common.h"
#include "logger.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <uv.h>

static http3_server_api *g_server = NULL;

/* ── signal handler ─────────────────────────── */
static void on_signal(uv_signal_t *sig, int signum) {
    LOG_INFO("[h3-echo] signal %d, shutting down", signum);
    uv_signal_stop(sig);
    if (g_server) {
        http3_server_api_destroy(g_server);
        g_server = NULL;
    }
    uv_stop(sig->loop);
}

/* ── GET /get/hello ──────────────────────────── */
static void handle_get_hello(http3_request *req, http3_response *resp) {
    resp->status_code = 200;

    const char *name = http3_kv_val(req->query, "name");
    const char *age  = http3_kv_val(req->query, "age");

    char buf[8192];
    int n = snprintf(buf, sizeof(buf),
        "{\n"
        "  \"greeting\": \"Hello, %s!\",\n"
        "  \"age\": %s\n"
        "}\n",
        name ? name : "guest",
        age  ? age  : "unknown");
    resp->write(resp, buf, n);
}

/* ── POST /post/hello ────────────────────────── */
static void handle_post_hello(http3_request *req, http3_response *resp) {
    resp->status_code = 200;

    const char *content_type = http3_kv_val(req->headers, "content-type");
    const char *content_len  = http3_kv_val(req->headers, "content-length");
    const char *body = (req->body_len > 0) ? (const char*)req->body : "";

    char buf[8192];
    int n = snprintf(buf, sizeof(buf),
        "{\n"
        "  \"body\": \"%s\",\n"
        "  \"content-type\": \"%s\",\n"
        "  \"content-length\": \"%s\"\n"
        "}\n",
        body,
        content_type ? content_type : "(curl did not send this header)",
        content_len  ? content_len  : "0");
    resp->write(resp, buf, n);
}

/* ── WT session datagram callback ─────────────── */
static void on_wt_dgram(webtransport_session *session,
                          const uint8_t *data, size_t len) {
    const uint8_t *payload = data + 1;
    size_t payload_len = len > 0 ? len - 1 : 0;
    LOG_INFO("[h3-echo] WT dgram %zu bytes: '%.*s'",
             payload_len, (int)payload_len,
             payload_len > 0 ? (const char*)payload : "");
    webtransport_session_send_datagram(session, data, len);
}

/* ── QUIC varint decoder ──────────────────────── */
static size_t quic_varint_read(const uint8_t *data, size_t len,
                                uint64_t *value) {
    if (len < 1) return 0;
    size_t vl;
    switch (data[0] & 0xc0) {
        case 0x00: vl = 1; break;  /* 00xxxxxx */
        case 0x40: vl = 2; break;  /* 01xxxxxx */
        case 0x80: vl = 4; break;  /* 10xxxxxx */
        default:   vl = 8; break;  /* 11xxxxxx */
    }
    if (vl > len) return 0;
    *value = data[0] & 0x3F;
    for (size_t i = 1; i < vl; i++)
        *value = (*value << 8) | data[i];
    return vl;
}

/* ── WT session stream callback ───────────────── */
static void on_wt_stream(webtransport_session *session,
                          uint64_t stream_id, int is_uni,
                          const uint8_t *data, size_t len, int fin) {
    LOG_INFO("[h3-echo] WT stream %llu %zu bytes%s: %.*s",
             (unsigned long long)stream_id, len, fin?" FIN":"",
             (int)(len < 80 ? len : 80), data);

    /* Strip WebTransport STREAM frame header (RFC 9220 §5.1).
     * Format: Frame Type (i) = 0x41..0x5f, Session ID (i).
     * Chrome sends this prefix on every new stream — both bidi and uni. */
    const uint8_t *payload = data;
    size_t payload_len = len;
    if (len > 0) {
        uint64_t frame_type;
        size_t ft_len = quic_varint_read(data, len, &frame_type);
        if (ft_len > 0 && frame_type >= 0x41 && frame_type <= 0x5f) {
            uint64_t session_id;
            size_t sid_len = quic_varint_read(data + ft_len,
                                               len - ft_len, &session_id);
            if (sid_len > 0) {
                size_t header_len = ft_len + sid_len;
                payload      = data + header_len;
                payload_len  = len - header_len;
                LOG_INFO("[h3-echo] WT header: type=0x%llx session=%llu"
                         " (stripped %zu)",
                         (unsigned long long)frame_type,
                         (unsigned long long)session_id, header_len);
            }
        }
    }

    if (payload_len > 0) {
        int ret = webtransport_session_send_stream_data(session, stream_id,
                                                         payload, payload_len, fin);
        if (ret < 0) {
            LOG_ERROR("[h3-echo] echo send failed (flow control?), dropped %zu bytes",
                      payload_len);
        } else {
            LOG_INFO("[h3-echo] echo %zu bytes on stream %llu",
                     payload_len, (unsigned long long)stream_id);
        }
    } else {
        LOG_INFO("[h3-echo] header-only write, skip echo (stream %llu)",
                 (unsigned long long)stream_id);
    }
}

/* ── WebTransport session callback ────────────── */
static void on_wt_session(http3_server_api *srv,
                           webtransport_session *session,
                           const char *path) {
    LOG_INFO("[h3-echo] WebTransport session %llu on path %s",
             (unsigned long long)webtransport_session_get_id(session),
             path ? path : "/");
    webtransport_session_set_on_datagram(session, on_wt_dgram);
    webtransport_session_set_on_stream_data(session, on_wt_stream);
}

int main(int argc, char *argv[]) {
    setbuf(stdout, NULL);
    setbuf(stderr, NULL);

    /* 扫描 --aes256 / --chacha20 */
    const char *cert_file = NULL, *key_file = NULL, *ip = NULL;
    int port = 4433;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--aes256") == 0)
            tls_set_cipher_hint("TLS_AES_256_GCM_SHA384");
        else if (strcmp(argv[i], "--chacha20") == 0)
            tls_set_cipher_hint("TLS_CHACHA20_POLY1305_SHA256");
        else if (!cert_file) cert_file = argv[i];
        else if (!key_file)  key_file  = argv[i];
        else if (!ip)        ip        = argv[i];
        else                 port      = atoi(argv[i]);
    }
    if (!cert_file) cert_file = "../cert/server_cert.pem";
    if (!key_file)  key_file  = "../cert/server_key.pem";
    if (!ip)        ip        = "127.0.0.1";

    uv_loop_t *loop = uv_default_loop();

    uv_signal_t sig;
    uv_signal_init(loop, &sig);
    uv_signal_start(&sig, on_signal, SIGINT);

    log_set_level(INFO);
    log_set_console(0);
    log_set_file("/tmp/http3_echo_server.log");

    /* ── 创建 server + 注册路由 ── */
    g_server = http3_server_api_create(loop, cert_file, key_file, ip, port);
    if (!g_server) {
        LOG_ERROR("[h3-echo] server create failed");
        return 1;
    }

    http3_server_api_add_handler(g_server, HTTP3_GET, "/get/hello", handle_get_hello);
    http3_server_api_add_handler(g_server, HTTP3_POST, "/post/hello", handle_post_hello);

    /* WebTransport session callback */
    http3_server_api_set_on_wt_session(g_server, on_wt_session);

    LOG_INFO("[h3-echo] ===============================================");
    LOG_INFO("[h3-echo] HTTP/3 Echo Server starting...");
    LOG_INFO("[h3-echo] Built: " __DATE__ " " __TIME__);
    LOG_INFO("[h3-echo] PID=%d", (int)getpid());
    LOG_INFO("[h3-echo] listening on %s:%d (press Ctrl+C to stop)", ip, port);
    LOG_INFO("[h3-echo] ===============================================");

    uv_run(loop, UV_RUN_DEFAULT);

    /* 清理 */
    if (g_server) http3_server_api_destroy(g_server);
    g_server = NULL;

    uv_signal_stop(&sig);
    uv_close((uv_handle_t*)&sig, NULL);
    uv_run(loop, UV_RUN_NOWAIT);

    uv_loop_close(loop);
    quic_crypto_cleanup();
    log_shutdown();
    return 0;
}
