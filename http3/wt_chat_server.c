#include "webtransport_server_api.h"
#include "logger.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uv.h>

/* ============================================================
 * WebTransport Chat Server — /chat?roomid=xxx
 *
 * 消息协议（UTF-8 JSON）：
 *   上行聊天：{"user":"<name>","msg":"<text>"}
 *   上行心跳：{"user":"<name>","type":"heartbeat"}
 *   下行聊天：{"from":"<name>","msg":"<text>"}
 *   下行成员：
 *     {"from":"system","type":"members","users":["a","b"],"msg":"..."}
 *     {"from":"system","type":"join","user":"a","msg":"a joined"}
 *     {"from":"system","type":"leave","user":"a","msg":"a left"}
 *
 * 成员在「首条上行且带 user」后正式上线（announced）：
 *   - 给本人推送当前 members 快照
 *   - 给其他人推送 join
 * 离开 / 心跳超时：推送 leave。
 * ============================================================ */

#define CHAT_HB_TIMEOUT_MS   20000
#define CHAT_HB_CHECK_MS      2000

static wt_server_t *g_srv = NULL;
static uv_loop_t   *g_loop = NULL;
static uv_timer_t   g_hb_timer;

typedef struct room_member {
    wt_session_t        *sess;
    wt_stream_t         *stream;
    char                 user[64];
    uint64_t             last_active_ms;
    int                  announced; /* 1 = 已正式上线并通知过房间 */
    struct room_member  *next;
} room_member;

typedef struct chat_room {
    char              roomid[64];
    room_member      *members;
    struct chat_room *next;
} chat_room;

static chat_room *g_rooms = NULL;

static chat_room *room_find(const char *roomid) {
    for (chat_room *r = g_rooms; r; r = r->next)
        if (strcmp(r->roomid, roomid) == 0)
            return r;
    return NULL;
}

static chat_room *room_get_or_create(const char *roomid) {
    chat_room *r = room_find(roomid);
    if (r) return r;

    r = (chat_room*)calloc(1, sizeof(*r));
    if (!r) return NULL;
    snprintf(r->roomid, sizeof(r->roomid), "%s", roomid);
    r->next = g_rooms;
    g_rooms = r;
    LOG_INFO("[chat] room created: %s", roomid);
    return r;
}

static room_member *room_find_member(chat_room *r, wt_session_t *sess) {
    for (room_member *m = r->members; m; m = m->next)
        if (m->sess == sess) return m;
    return NULL;
}

static void room_add_member(chat_room *r, wt_session_t *sess, const char *user) {
    room_member *m = (room_member*)calloc(1, sizeof(*m));
    if (!m) return;
    m->sess = sess;
    snprintf(m->user, sizeof(m->user), "%s", user);
    m->last_active_ms = g_loop ? uv_now(g_loop) : 0;
    m->announced = 0;
    m->next = r->members;
    r->members = m;
}

static void room_remove_member(chat_room *r, wt_session_t *sess) {
    room_member **pp = &r->members;
    while (*pp) {
        if ((*pp)->sess == sess) {
            room_member *victim = *pp;
            *pp = victim->next;
            free(victim);
            return;
        }
        pp = &(*pp)->next;
    }
}

static void room_send_raw(room_member *m, const char *json, size_t len) {
    if (!m || !m->stream || !json || !len) return;
    wt_stream_write(m->stream, (const uint8_t*)json, len);
}

/* 构造 members 快照。只包含已 announced 且有 stream 的成员。 */
static int room_build_members_json(chat_room *r, char *out, size_t cap) {
    size_t p = 0;
    int n = snprintf(out + p, cap - p,
                     "{\"from\":\"system\",\"type\":\"members\",\"users\":[");
    if (n < 0 || (size_t)n >= cap - p) return -1;
    p += (size_t)n;

    int first = 1;
    for (room_member *m = r->members; m; m = m->next) {
        if (!m->announced || !m->stream || !m->user[0]) continue;
        n = snprintf(out + p, cap - p, "%s\"%s\"", first ? "" : ",", m->user);
        if (n < 0 || (size_t)n >= cap - p) return -1;
        p += (size_t)n;
        first = 0;
    }

    n = snprintf(out + p, cap - p, "],\"msg\":\"online members\"}");
    if (n < 0 || (size_t)n >= cap - p) return -1;
    p += (size_t)n;
    return (int)p;
}

static void room_send_members_snapshot(chat_room *r, room_member *to) {
    char out[4096];
    int n = room_build_members_json(r, out, sizeof(out));
    if (n < 0) return;
    LOG_INFO("[chat] members snapshot -> %s: %s", to->user, out);
    room_send_raw(to, out, (size_t)n);
}

/* 广播给已上线成员；exclude 可为 NULL。 */
static void room_broadcast_raw(chat_room *r, const char *json, size_t len,
                                wt_session_t *exclude) {
    for (room_member *m = r->members; m; m = m->next) {
        if (!m->announced || !m->stream) continue;
        if (exclude && m->sess == exclude) continue;
        LOG_INFO("[chat] event -> %s: %s", m->user, json);
        room_send_raw(m, json, len);
    }
}

static void room_broadcast_join(chat_room *r, const char *user,
                                 wt_session_t *exclude) {
    char out[512];
    int n = snprintf(out, sizeof(out),
        "{\"from\":\"system\",\"type\":\"join\",\"user\":\"%s\",\"msg\":\"%s joined\"}",
        user, user);
    if (n < 0 || (size_t)n >= sizeof(out)) return;
    room_broadcast_raw(r, out, (size_t)n, exclude);
}

static void room_broadcast_leave(chat_room *r, const char *user,
                                  const char *reason) {
    char out[512];
    int n = snprintf(out, sizeof(out),
        "{\"from\":\"system\",\"type\":\"leave\",\"user\":\"%s\",\"msg\":\"%s\"}",
        user, reason);
    if (n < 0 || (size_t)n >= sizeof(out)) return;
    room_broadcast_raw(r, out, (size_t)n, NULL);
}

static void room_broadcast_chat(chat_room *r, const char *from, const char *msg) {
    char out[4096];
    int n = snprintf(out, sizeof(out), "{\"from\":\"%s\",\"msg\":\"%s\"}",
                     from, msg);
    if (n < 0 || (size_t)n >= sizeof(out)) return;

    for (room_member *m = r->members; m; m = m->next) {
        if (!m->announced || !m->stream) continue;
        if (strcmp(m->user, from) == 0) continue;
        LOG_INFO("[chat] broadcast from %s to %s: %s", from, m->user, out);
        room_send_raw(m, out, (size_t)n);
    }
}

static int json_extract(const char *json, const char *key,
                        char *out, size_t out_cap) {
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\":\"", key);
    const char *p = strstr(json, pat);
    if (!p) return -1;
    p += strlen(pat);
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < out_cap)
        out[i++] = *p++;
    out[i] = '\0';
    return 0;
}

/* 正式上线：发 members 给自己，join 给其他人 */
static void member_announce_if_needed(chat_room *r, room_member *sender) {
    if (!sender || sender->announced) return;
    if (!sender->stream || !sender->user[0]) return;
    /* 临时占位名不算正式上线 */
    if (strncmp(sender->user, "user-", 5) == 0) return;

    sender->announced = 1;
    LOG_INFO("[chat] announced room=%s user=%s", r->roomid, sender->user);

    room_send_members_snapshot(r, sender);
    room_broadcast_join(r, sender->user, sender->sess);
}

static void on_hb_timer(uv_timer_t *t) {
    (void)t;
    if (!g_loop) return;
    uint64_t now = uv_now(g_loop);

    for (chat_room *r = g_rooms; r; r = r->next) {
        room_member *m = r->members;
        while (m) {
            room_member *next = m->next;
            if (m->last_active_ms != 0 &&
                now > m->last_active_ms &&
                (now - m->last_active_ms) >= CHAT_HB_TIMEOUT_MS) {
                char uname[64];
                snprintf(uname, sizeof(uname), "%s", m->user);
                int was_announced = m->announced;
                wt_session_t *sess = m->sess;

                LOG_WARN("[chat] heartbeat timeout room=%s user=%s idle=%llums",
                         r->roomid, uname,
                         (unsigned long long)(now - m->last_active_ms));

                room_remove_member(r, sess);

                if (was_announced) {
                    char reason[128];
                    snprintf(reason, sizeof(reason), "%s left (timeout)", uname);
                    room_broadcast_leave(r, uname, reason);
                }

                if (sess) wt_session_close(sess);
            }
            m = next;
        }
    }
}

static void on_session(wt_server_t *srv, wt_session_t *sess, void *user) {
    (void)srv; (void)user;

    const char *roomid = wt_session_get_param(sess, "roomid");
    if (!roomid || !*roomid) roomid = "default";

    char uname[64];
    snprintf(uname, sizeof(uname), "user-%s-%p", roomid, (void*)sess);

    chat_room *r = room_get_or_create(roomid);
    if (!r) return;
    room_add_member(r, sess, uname);
    wt_session_set_user_data(sess, r);

    LOG_INFO("[chat] session open room=%s placeholder=%s", roomid, uname);
    /* 等客户端首条带真实 user 的上行后再 announce */
}

static void on_stream_data(wt_server_t *srv, wt_session_t *sess,
                            wt_stream_t *st,
                            const uint8_t *data, size_t len, void *user) {
    (void)srv; (void)user;

    chat_room *r = (chat_room*)wt_session_get_user_data(sess);
    if (!r) return;

    room_member *sender = room_find_member(r, sess);
    if (!sender) return;

    if (!sender->stream)
        sender->stream = st;

    if (g_loop)
        sender->last_active_ms = uv_now(g_loop);

    char buf[4096];
    size_t n = len < sizeof(buf) - 1 ? len : sizeof(buf) - 1;
    memcpy(buf, data, n);
    buf[n] = '\0';

    char uname[64], msg[4096], typ[32];
    if (json_extract(buf, "user", uname, sizeof(uname)) == 0) {
        snprintf(sender->user, sizeof(sender->user), "%s", uname);
    }

    member_announce_if_needed(r, sender);

    if (json_extract(buf, "type", typ, sizeof(typ)) == 0 &&
        strcmp(typ, "heartbeat") == 0) {
        LOG_DEBUG("[chat] heartbeat room=%s user=%s", r->roomid, sender->user);
        return;
    }

    if (json_extract(buf, "msg", msg, sizeof(msg)) != 0) {
        snprintf(msg, sizeof(msg), "%s", buf);
    }

    LOG_INFO("[chat] room=%s %s: %s", r->roomid, sender->user, msg);
    room_broadcast_chat(r, sender->user, msg);
}

static void on_session_close(wt_server_t *srv, wt_session_t *sess, void *user) {
    (void)srv; (void)user;

    chat_room *r = (chat_room*)wt_session_get_user_data(sess);
    if (!r) {
        LOG_WARN("[chat] session close without room");
        return;
    }

    room_member *m = room_find_member(r, sess);
    if (!m) {
        LOG_WARN("[chat] session close without member");
        return;
    }

    char uname[64];
    snprintf(uname, sizeof(uname), "%s", m->user);
    int was_announced = m->announced;

    room_remove_member(r, sess);
    LOG_INFO("[chat] leave room=%s member=%s", r->roomid, uname);

    if (was_announced) {
        char reason[128];
        snprintf(reason, sizeof(reason), "%s left", uname);
        room_broadcast_leave(r, uname, reason);
    }
}

static void on_keepalive(http3_request *req, http3_response *resp) {
    char body[512];
    size_t n = req->body_len < (int)sizeof(body) - 1
             ? (size_t)req->body_len : sizeof(body) - 1;
    memcpy(body, req->body, n);
    body[n] = '\0';

    LOG_INFO("[chat] keepalive: %s", body);

    resp->status_code = 200;
    resp->write(resp, "ok", 2);
}

static void on_signal(uv_signal_t *sig, int signum) {
    LOG_WARN("[chat] signal %d, shutting down", signum);
    uv_timer_stop(&g_hb_timer);
    if (g_srv) wt_server_destroy(g_srv);
    g_srv = NULL;
    uv_stop(sig->loop);
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
    if (!ip)        ip        = "0.0.0.0";

    setbuf(stdout, NULL);
    log_set_level(INFO);
    log_set_console(1);
    log_set_file("/tmp/wt_chat_server.log");

    g_loop = uv_default_loop();
    uv_signal_t sig; uv_signal_init(g_loop, &sig);
    uv_signal_start(&sig, on_signal, SIGINT);

    g_srv = wt_server_create(g_loop);
    if (!g_srv) { LOG_ERROR("[chat] create failed"); return 1; }

    wt_path_callbacks_t cb = {0};
    cb.on_session      = on_session;
    cb.on_stream_data  = on_stream_data;
    cb.on_session_close = on_session_close;
    wt_server_add_path(g_srv, "/chat", &cb);

    wt_server_add_http_handler(g_srv, HTTP3_POST, "/keepalive", on_keepalive);

    if (wt_server_listen(g_srv, cert_file, key_file, ip, port) < 0) {
        LOG_ERROR("[chat] listen failed"); return 1;
    }

    uv_timer_init(g_loop, &g_hb_timer);
    uv_timer_start(&g_hb_timer, on_hb_timer, CHAT_HB_CHECK_MS, CHAT_HB_CHECK_MS);

    LOG_WARN("[chat] listening on %s:%d (path=/chat, hb_timeout=%ds)",
             ip, port, CHAT_HB_TIMEOUT_MS / 1000);

    uv_run(g_loop, UV_RUN_DEFAULT);
    g_srv = NULL;
    log_shutdown();
    return 0;
}
