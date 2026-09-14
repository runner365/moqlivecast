#ifndef HTTP3_SERVER_API_H
#define HTTP3_SERVER_API_H

#include <stdint.h>
#include <stddef.h>
#include <uv.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================
 * HTTP/3 Server — 用户层 API
 * ============================================ */

typedef struct http3_server_api http3_server_api;
typedef struct http3_request     http3_request;
typedef struct http3_response    http3_response;
typedef struct webtransport_session webtransport_session;

/* ── HTTP 方法 ─────────────────────────────── */
typedef enum {
    HTTP3_GET,
    HTTP3_POST,
    HTTP3_PUT,
    HTTP3_DELETE,
    HTTP3_HEAD,
    HTTP3_OPTIONS,
    HTTP3_CONNECT,  /* WebTransport */
} http3_method;

/* ── 键值对链表 (query params / headers) ──── */
typedef struct http3_kv {
    char           *key;
    char           *value;
    struct http3_kv *next;
} http3_kv;

/* ── 请求 ──────────────────────────────────── */
struct http3_request {
    http3_method  method;
    const char   *path;        /* "?" 前的路径部分，如 "/hello" */
    http3_kv     *query;       /* query 参数链表，如 name=world&x=1 */
    http3_kv     *headers;     /* 全部请求头链表 */
    uint8_t      *body;        /* HTTP body 数据（可 NULL） */
    int           body_len;
    /* 内部使用 — 应用层不直接访问 */
    void         *_conn;
    uint64_t      _stream_id;
};

/* ── 响应 — 立即发送模式 ──────────────────── */

/* 首次调用 → 发送 HEADERS 帧；后续调用 → 发送 DATA 帧 */
typedef void (*http3_resp_write_fn)(http3_response *resp,
                                    const char *data, int len);

struct http3_response {
    int                 status_code;   /* HTTP 状态码，handler 需设置 */
    http3_resp_write_fn write;         /* 写响应体（可靠 STREAM）；首次调用自动发送 HEADERS */
    http3_resp_write_fn send_datagram; /* 发数据报（不可靠 DATAGRAM）；不触发 HEADERS */
    /* 内部使用 */
    void               *_conn;
    uint64_t            _stream_id;
    int                 _headers_sent;
};

/* ── Handler 回调 ──────────────────────────── */
typedef void (*http3_handler_fn)(http3_request *req, http3_response *resp);

/* ── Server API ────────────────────────────── */

/* 创建 HTTP/3 服务端，自动绑定并监听。
 * 失败返回 NULL；成功返回 handle。
 */
http3_server_api *http3_server_api_create(
    uv_loop_t  *loop,
    const char *cert_file,
    const char *key_file,
    const char *ip,
    int         port);

void http3_server_api_destroy(http3_server_api *srv);

/* 注册路由 handler — 精确匹配 method + path。
 * 返回 0 成功，-1 失败（内存不足）。
 */
int  http3_server_api_add_handler(http3_server_api *srv,
                                  http3_method       method,
                                  const char        *path,
                                  http3_handler_fn   callback);

/* ── DATAGRAM 回调 ────────────────────────── */

/* 收到不可靠数据报时的回调（RFC 9221 §4）。
 * 注册 on_datagram 后，datagram 到此回调，不会路由到 handler。 */
typedef void (*http3_on_datagram_fn)(http3_server_api *srv,
                                     const uint8_t *data, size_t len);

void http3_server_api_set_on_datagram(http3_server_api *srv,
                                       http3_on_datagram_fn cb);

/* ── WebTransport 回调 ────────────────────── */

/* CONNECT :protocol=webtransport 握手成功后调用 */
typedef void (*http3_on_wt_session_fn)(http3_server_api *srv,
                                        webtransport_session *session,
                                        const char *path);

void http3_server_api_set_on_wt_session(http3_server_api *srv,
                                         http3_on_wt_session_fn cb);

/* ── 公共工具 ──────────────────────────────── */

/* 遍历 http3_kv 链表，返回 key 对应的 value；
 * 未找到返回 NULL。适用于 req->headers 和 req->query。 */
const char *http3_kv_val(http3_kv *kv, const char *key);

#ifdef __cplusplus
}
#endif

#endif /* HTTP3_SERVER_API_H */
