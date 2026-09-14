#include "quic_stream.h"
#include "quic_packet.h"
#include "logger.h"
#include <stdlib.h>
#include <string.h>
#include <uv.h>

/* ============================================
 * Stream 查找 / 创建
 * ============================================ */

/* 对端 TP 给本 stream 的发送额度（RFC 9000 §4.1 / §18.2）。
 * lim=0 表示尚未得知额度，禁止发送，等 MAX_STREAM_DATA / apply_init_send_credit。
 * 绝不能臆造默认窗口，否则会触发对端 FLOW_CONTROL_ERROR。 */
static uint64_t peer_send_credit(QuicStreamCtx *ctx, uint64_t sid) {
    const int we_init = ((sid & 1ULL) == (ctx->next_local_bidi_id & 1ULL));
    const int uni = (sid & 2ULL) != 0;
    if (uni) return ctx->peer_init_max_stream_data_uni;
    if (we_init) return ctx->peer_init_max_stream_data_bidi_remote;
    return ctx->peer_init_max_stream_data_bidi_local;
}

static QuicStream* stream_find(QuicStreamCtx *ctx, uint64_t stream_id) {
    for (size_t i = 0; i < ctx->stream_cnt; i++) {
        if (ctx->streams[i] && ctx->streams[i]->stream_id == stream_id)
            return ctx->streams[i];
    }
    return NULL;
}

static QuicStream* stream_find_or_create(QuicStreamCtx *ctx, uint64_t stream_id) {
    QuicStream *s = stream_find(ctx, stream_id);
    if (s) return s;

    if (ctx->stream_cnt >= ctx->stream_cap) {
        size_t new_cap = ctx->stream_cap ? ctx->stream_cap * 2 : 4;
        QuicStream **new_arr = (QuicStream**)realloc(ctx->streams, new_cap * sizeof(QuicStream*));
        if (!new_arr) return NULL;
        ctx->streams = new_arr;
        ctx->stream_cap = new_cap;
    }

    s = (QuicStream*)calloc(1, sizeof(QuicStream));
    if (!s) return NULL;

    s->stream_id      = stream_id;
    s->send_state     = QUIC_STREAM_SEND_READY;
    s->recv_state     = QUIC_STREAM_RECV_RECV;
    s->recv_max_data  = QUIC_STREAM_RECV_BUF_SIZE;
    s->send_max_data  = peer_send_credit(ctx, stream_id);
    s->recv_buf_cap   = 0;

    ctx->streams[ctx->stream_cnt++] = s;
    return s;
}

/* ============================================
 * 公开 API — stream open
 * ============================================ */

uint64_t quic_stream_open(QuicStreamCtx *ctx, void *conn,
                           int (*queue_fn)(void *conn, const uint8_t *frames, size_t flen)) {
    uint64_t sid = ctx->next_local_bidi_id;
    uint64_t stream_count = sid / 4;
    if (stream_count >= ctx->max_streams_bidi_peer &&
        ctx->max_streams_bidi_peer > 0) {
        LOG_DEBUG("[quic-stream] bidi stream limit reached: %llu/%llu",
                 (unsigned long long)stream_count,
                 (unsigned long long)ctx->max_streams_bidi_peer);
        uint8_t frm[16];
        int flen = quic_frame_write_streams_blocked(frm, sizeof(frm),
                                                     ctx->max_streams_bidi_peer, 1);
        if (flen > 0) queue_fn(conn, frm, (size_t)flen);
        return UINT64_MAX;
    }
    ctx->next_local_bidi_id += 4;
    QuicStream *s = stream_find_or_create(ctx, sid);
    if (!s) return UINT64_MAX;
    s->send_state = QUIC_STREAM_SEND_SEND;
    LOG_DEBUG("[quic-stream] opened bidi stream %llu", (unsigned long long)sid);
    return sid;
}

uint64_t quic_stream_open_uni(QuicStreamCtx *ctx, void *conn,
                               int (*queue_fn)(void *conn, const uint8_t *frames, size_t flen)) {
    uint64_t sid = ctx->next_local_uni_id;
    uint64_t stream_count = sid / 4;
    if (stream_count >= ctx->max_streams_uni_peer &&
        ctx->max_streams_uni_peer > 0) {
        LOG_DEBUG("[quic-stream] uni stream limit reached: %llu/%llu",
                 (unsigned long long)stream_count,
                 (unsigned long long)ctx->max_streams_uni_peer);
        uint8_t frm[16];
        int flen = quic_frame_write_streams_blocked(frm, sizeof(frm),
                                                     ctx->max_streams_uni_peer, 0);
        if (flen > 0) queue_fn(conn, frm, (size_t)flen);
        return UINT64_MAX;
    }
    ctx->next_local_uni_id += 4;
    QuicStream *s = stream_find_or_create(ctx, sid);
    if (!s) return UINT64_MAX;
    s->send_state     = QUIC_STREAM_SEND_SEND;
    s->recv_state     = QUIC_STREAM_RECV_RESET_RECVD;
    s->recv_max_data  = 0;
    s->send_max_data  = peer_send_credit(ctx, sid);
    LOG_DEBUG("[quic-stream] opened uni stream %llu", (unsigned long long)sid);
    return sid;
}

/* ============================================
 * Write request 链表
 * ============================================ */

/* 按 start_offset 升序插入 */
static void req_insert(QuicStream *s, quic_stream_write_req_t *req) {
    quic_stream_write_req_t **pp = &s->write_reqs;
    while (*pp && (*pp)->start_offset < req->start_offset)
        pp = &(*pp)->next;
    req->next = *pp;
    *pp = req;
}

/* 发送偏移推进后，回调所有已完成的 req */
static void req_fire_completed(QuicStream *s, uint64_t acked_offset) {
    while (s->write_reqs) {
        quic_stream_write_req_t *r = s->write_reqs;
        if (acked_offset < r->start_offset + r->length) break;
        s->write_reqs = r->next;
        if (r->cb) r->cb(s, 0, r->length, r->user_data);
        free(r);
    }
}

/* 清空所有 pending req 并回调失败 */
static void req_fail_all(QuicStream *s, int ret_code) {
    while (s->write_reqs) {
        quic_stream_write_req_t *r = s->write_reqs;
        s->write_reqs = r->next;
        if (r->cb) r->cb(s, ret_code, 0, r->user_data);
        free(r);
    }
}

/* ============================================
 * send_buf 空间管理 & 数据追加
 * ============================================ */

/* 确保 send_buf 有 total_needed 字节空间（头 + 内容），必要时整理/扩容 */
static int send_buf_ensure(QuicStream *s, size_t total_needed) {
    if (total_needed <= s->send_buf_cap) {
        /* 尾部空间不够但总容量够 → memmove 整理 */
        if (s->send_buf_head + total_needed > s->send_buf_cap) {
            memmove(s->send_buf, s->send_buf + s->send_buf_head, s->send_buf_len);
            s->send_buf_head = 0;
        }
        return 0;
    }

    /* 整理后再试扩容 */
    if (s->send_buf_head > 0 && s->send_buf_len + total_needed <= s->send_buf_cap) {
        memmove(s->send_buf, s->send_buf + s->send_buf_head, s->send_buf_len);
        s->send_buf_head = 0;
        return 0;
    }

    /* 必须扩容 */
    size_t new_cap = s->send_buf_cap ? s->send_buf_cap * 2 : 4096;
    while (new_cap < s->send_buf_head + total_needed) new_cap *= 2;
    uint8_t *nb = (uint8_t*)realloc(s->send_buf, new_cap);
    if (!nb) return -1;
    s->send_buf = nb;
    s->send_buf_cap = new_cap;
    return 0;
}

static int send_buf_append(QuicStream *s, const uint8_t *data, size_t len) {
    if (len == 0) return 0;
    if (send_buf_ensure(s, s->send_buf_len + len) < 0) return -1;
    memcpy(s->send_buf + s->send_buf_head + s->send_buf_len, data, len);
    s->send_buf_len += len;
    return 0;
}

static void maybe_notify_writable(QuicStream *s) {
    if (!s || !s->want_writable) return;
    if (s->send_buf_len > QUIC_STREAM_SEND_LOW_WATER) return;
    s->want_writable = 0;
    if (s->on_writable) s->on_writable(s, s->on_writable_user);
}

/* ============================================
 * 从 send_buf 消费 & 回调
 * ============================================ */

#define MAX_FRAME_BUF     2048
#define MAX_PAYLOAD_CHUNK 1200

static int consume_send_buf(QuicStreamCtx *ctx, void *conn, QuicStream *s,
                             int (*queue_fn)(void *conn, const uint8_t *frames, size_t flen),
                             void (*flush_fn)(void *conn)) {
    while (s->send_buf_len > 0) {
        /* 剩余发送额度（RFC 9000 §4.1）：绝对不能让 offset+len 越过上限 */
        uint64_t stream_room = (s->send_offset < s->send_max_data)
                             ? (s->send_max_data - s->send_offset) : 0;
        uint64_t conn_room = (ctx->bytes_sent < ctx->max_data_peer)
                           ? (ctx->max_data_peer - ctx->bytes_sent) : 0;
        /* 诊断：每次尝试消费都记录卡点（分钟级限流），定位是哪个闸门 */
        {
            static uint64_t dbg_last_s = 0;
            uint64_t now_s = uv_now(uv_default_loop()) / 1000;
            if (now_s != dbg_last_s) {
                dbg_last_s = now_s;
                LOG_DEBUG("[quic-stream] consume stream=%llu pending=%zu "
                         "stream_room=%llu (off=%llu max=%llu) "
                         "conn_room=%llu (sent=%llu peer_max=%llu)",
                         (unsigned long long)s->stream_id, s->send_buf_len,
                         (unsigned long long)stream_room,
                         (unsigned long long)s->send_offset,
                         (unsigned long long)s->send_max_data,
                         (unsigned long long)conn_room,
                         (unsigned long long)ctx->bytes_sent,
                         (unsigned long long)ctx->max_data_peer);
            }
        }
        if (stream_room == 0) {
            const int already = s->send_blocked;
            s->send_blocked = 1;
            LOG_DEBUG("[quic-stream] consume blocked: STREAM FC id=%llu off=%llu max=%llu",
                     (unsigned long long)s->stream_id, (unsigned long long)s->send_offset, (unsigned long long)s->send_max_data);
            if (!already) {
                LOG_DEBUG("[quic-stream] stream FC blocked id=%llu off=%llu max=%llu pending=%zu",
                         (unsigned long long)s->stream_id,
                         (unsigned long long)s->send_offset,
                         (unsigned long long)s->send_max_data,
                         s->send_buf_len);
                uint8_t frm[32];
                int flen = quic_frame_write_stream_data_blocked(frm, sizeof(frm),
                                                                s->stream_id,
                                                                s->send_max_data);
                if (flen > 0) { queue_fn(conn, frm, (size_t)flen); flush_fn(conn); }
            }
            maybe_notify_writable(s);
            return -1;
        }
        if (conn_room == 0) {
            const int already = s->send_blocked;
            s->send_blocked = 1;
            LOG_DEBUG("[quic-stream] CONN FC BLOCKED stream=%llu sent=%llu peer_max=%llu pending=%zu",
                     (unsigned long long)s->stream_id,
                     (unsigned long long)ctx->bytes_sent,
                     (unsigned long long)ctx->max_data_peer,
                     s->send_buf_len);
            if (!already) {
                LOG_DEBUG("[quic-stream] conn FC blocked stream=%llu "
                         "sent=%llu max_data_peer=%llu pending=%zu",
                         (unsigned long long)s->stream_id,
                         (unsigned long long)ctx->bytes_sent,
                         (unsigned long long)ctx->max_data_peer,
                         s->send_buf_len);
                uint8_t frm[16];
                int flen = quic_frame_write_data_blocked(frm, sizeof(frm),
                                                          ctx->max_data_peer);
                if (flen > 0) { queue_fn(conn, frm, (size_t)flen); flush_fn(conn); }
            }
            maybe_notify_writable(s);
            return -1;
        }

        size_t chunk = s->send_buf_len;
        if (chunk > MAX_PAYLOAD_CHUNK) chunk = MAX_PAYLOAD_CHUNK;
        if (chunk > stream_room) chunk = (size_t)stream_room;
        if (chunk > conn_room) chunk = (size_t)conn_room;
        if (chunk == 0) return -1;
        int is_last = (chunk >= s->send_buf_len) ? s->send_buf_fin : 0;

        uint8_t frame[MAX_FRAME_BUF];
        int flen = quic_frame_write_stream(frame, sizeof(frame),
                                           s->stream_id, s->send_offset,
                                           is_last,
                                           s->send_buf + s->send_buf_head, chunk);
        if (flen < 0) {
            LOG_ERROR("[quic-stream] frame encode failed (chunk=%zu)", chunk);
            return -1;
        }

        if (queue_fn(conn, frame, (size_t)flen) < 0) {
            s->send_blocked = 1;
            LOG_DEBUG("[quic-stream] stream %llu blocked: CWND full, %zu pending",
                      (unsigned long long)s->stream_id, s->send_buf_len);
            maybe_notify_writable(s);
            return -1;
        }

        s->send_offset  += chunk;
        ctx->bytes_sent += chunk;

        LOG_DEBUG("[quic-stream] consume %zuB stream=%llu off=%llu reqs=%p",
                 chunk, (unsigned long long)s->stream_id,
                 (unsigned long long)s->send_offset, (void*)s->write_reqs);

        /* 推进 head（不 memmove）。回调在 ACK 到达后触发 */
        s->send_buf_head += chunk;
        s->send_buf_len  -= chunk;
        s->send_blocked   = 0;
    }

    /* send_buf 清空 → 释放内存，重置 head */
    free(s->send_buf);
    s->send_buf = NULL;
    s->send_buf_head = 0;
    s->send_buf_cap  = 0;
    s->send_buf_fin  = 0;
    s->send_blocked  = 0;
    maybe_notify_writable(s);
    return 0;
}

#undef MAX_PAYLOAD_CHUNK
#undef MAX_FRAME_BUF

/* ============================================
 * 公开 API — 发送（带回调）
 * ============================================ */

int quic_stream_write(QuicStreamCtx *ctx, void *conn, uint64_t stream_id,
                       const uint8_t *data, size_t len, int fin,
                       quic_stream_write_cb cb, void *user_data,
                       uint64_t timeout_ms,
                       int (*queue_fn)(void *conn, const uint8_t *frames, size_t flen),
                       void (*flush_fn)(void *conn)) {
    QuicStream *s = stream_find(ctx, stream_id);
    if (!s) {
        LOG_ERROR("[quic-stream] write on unknown stream %llu", (unsigned long long)stream_id);
        if (cb) cb(NULL, -1, 0, user_data);
        return -1;
    }
    if (s->send_state == QUIC_STREAM_SEND_RESET_SENT ||
        s->send_state == QUIC_STREAM_SEND_DATA_SENT) {
        if (cb) cb(s, -1, 0, user_data);
        return -1;
    }

    /* 水位背压：本次整段不接受，由上层队列保留 */
    if (len > 0 && s->send_buf_len + len > QUIC_STREAM_SEND_HIGH_WATER) {
        s->want_writable = 1;
        LOG_DEBUG("[quic-stream] write congested stream=%llu pending=%zu +%zu high=%u, stream_id:%llu",
                  (unsigned long long)stream_id, s->send_buf_len, len,
                  (unsigned)QUIC_STREAM_SEND_HIGH_WATER, (unsigned long long)s->stream_id);
        return 1;
    }

    /* 创建 write_req（在数据附加到 send_buf 之前记录偏移） */
    quic_stream_write_req_t *req = NULL;
    if (cb) {
        req = (quic_stream_write_req_t*)calloc(1, sizeof(*req));
        if (!req) return -1;
        req->start_offset = s->send_offset + s->send_buf_len;
        req->length       = len;
        req->cb           = cb;
        req->user_data    = user_data;
        req->deadline_ms  = timeout_ms ? uv_now(uv_default_loop()) + timeout_ms : 0;
        req_insert(s, req);
    }

    /* 数据追加到 send_buf */
    if (len > 0 && send_buf_append(s, data, len) < 0) {
        /* 从链表移除刚插入的 req */
        if (req) {
            quic_stream_write_req_t **pp = &s->write_reqs;
            while (*pp && *pp != req) pp = &(*pp)->next;
            if (*pp) *pp = req->next;
            free(req);
        }
        return -1;
    }
    if (fin) s->send_buf_fin = 1;
    if (s->send_state == QUIC_STREAM_SEND_READY)
        s->send_state = QUIC_STREAM_SEND_SEND;

    /* 立即尝试消费 */
    consume_send_buf(ctx, conn, s, queue_fn, flush_fn);
    maybe_notify_writable(s);

    int64_t now_ms = uv_now(uv_default_loop());

    if (ctx->last_quic_stream_dbg_ts_s != now_ms / 1000) {
        ctx->last_quic_stream_dbg_ts_s = now_ms / 1000;
        LOG_DEBUG("[quic-stream] write stream %llu: %zu buffered offset=%llu, stream_id:%llu",
                  (unsigned long long)stream_id,
                  s->send_buf_len, (unsigned long long)s->send_offset, (unsigned long long)s->stream_id);
    }
    LOG_DEBUG("[quic-stream] write %zu bytes stream %llu: %zu buffered offset=%llu",
              len, (unsigned long long)stream_id,
              s->send_buf_len, (unsigned long long)s->send_offset);
    return 0;
}

/* 兼容旧接口 — 直接编码循环，不经过 send_buf。保持原有行为不变 */
int quic_stream_send(QuicStreamCtx *ctx, void *conn, uint64_t stream_id,
                      const uint8_t *data, size_t len, int fin,
                      int (*queue_fn)(void *conn, const uint8_t *frames, size_t flen),
                      void (*flush_fn)(void *conn)) {
    QuicStream *s = stream_find(ctx, stream_id);
    if (!s) { LOG_ERROR("[quic-stream] send unknown %llu", (unsigned long long)stream_id); return -1; }
    if (s->send_state == QUIC_STREAM_SEND_RESET_SENT) return -1;
    if (s->send_state == QUIC_STREAM_SEND_DATA_SENT) return -1;

    if (s->send_offset + len > s->send_max_data) {
        LOG_DEBUG("[quic-stream] stream fc blocked %llu", (unsigned long long)stream_id);
        uint8_t frm[32];
        int flen = quic_frame_write_stream_data_blocked(frm, sizeof(frm), stream_id, s->send_max_data);
        if (flen > 0) { queue_fn(conn, frm, (size_t)flen); flush_fn(conn); }
        return -1;
    }
    if (ctx->bytes_sent + len > ctx->max_data_peer) {
        LOG_DEBUG("[quic-stream] conn fc blocked");
        uint8_t frm[16];
        int flen = quic_frame_write_data_blocked(frm, sizeof(frm), ctx->max_data_peer);
        if (flen > 0) { queue_fn(conn, frm, (size_t)flen); flush_fn(conn); }
        return -1;
    }

    #define MC 1300
    if (len == 0 && fin) {
        uint8_t frame[2048];
        int flen = quic_frame_write_stream(frame, sizeof(frame), stream_id, s->send_offset, 1, NULL, 0);
        if (flen < 0) return -1;
        if (queue_fn(conn, frame, (size_t)flen) < 0) return -1;
    } else {
        size_t sent = 0;
        while (sent < len) {
            uint64_t stream_room = (s->send_offset < s->send_max_data)
                                 ? (s->send_max_data - s->send_offset) : 0;
            uint64_t conn_room = (ctx->bytes_sent < ctx->max_data_peer)
                               ? (ctx->max_data_peer - ctx->bytes_sent) : 0;
            if (stream_room == 0 || conn_room == 0) {
                LOG_DEBUG("[quic-stream] send fc blocked mid-write %llu",
                          (unsigned long long)stream_id);
                return -1;
            }
            size_t chunk = len - sent;
            if (chunk > MC) chunk = MC;
            if (chunk > stream_room) chunk = (size_t)stream_room;
            if (chunk > conn_room) chunk = (size_t)conn_room;
            if (chunk == 0) return -1;
            int is_last = (sent + chunk >= len) ? fin : 0;
            uint8_t frame[2048];
            int flen = quic_frame_write_stream(frame, sizeof(frame), stream_id, s->send_offset, is_last, data + sent, chunk);
            if (flen < 0) return -1;
            if (queue_fn(conn, frame, (size_t)flen) < 0) return -1;
            s->send_offset += chunk;
            ctx->bytes_sent += chunk;
            sent += chunk;
        }
    }
    #undef MC
    if (fin) s->send_state = QUIC_STREAM_SEND_DATA_SENT;
    else if (s->send_state == QUIC_STREAM_SEND_READY) s->send_state = QUIC_STREAM_SEND_SEND;
    return 0;
}

/* ============================================
 * 关闭 / 重置 / 停止
 * ============================================ */

void quic_stream_close_send(QuicStreamCtx *ctx, void *conn, uint64_t stream_id,
                             int (*queue_fn)(void *conn, const uint8_t *frames, size_t flen),
                             void (*flush_fn)(void *conn)) {
    QuicStream *s = stream_find(ctx, stream_id);
    if (!s || s->send_state == QUIC_STREAM_SEND_DATA_SENT) return;

    uint8_t frame[32];
    int flen = quic_frame_write_stream(frame, sizeof(frame),
                                       stream_id, s->send_offset, 1, NULL, 0);
    if (flen > 0) { queue_fn(conn, frame, (size_t)flen); flush_fn(conn); }
    s->send_state = QUIC_STREAM_SEND_DATA_SENT;
}

int quic_stream_reset(QuicStreamCtx *ctx, void *conn, uint64_t stream_id,
                       uint64_t error_code,
                       int (*queue_fn)(void *conn, const uint8_t *frames, size_t flen),
                       void (*flush_fn)(void *conn)) {
    QuicStream *s = stream_find(ctx, stream_id);
    if (!s) return -1;

    uint8_t frame[64];
    int flen = quic_frame_write_reset_stream(frame, sizeof(frame),
                                             stream_id, error_code,
                                             s->send_offset);
    if (flen < 0) return -1;

    queue_fn(conn, frame, (size_t)flen);
    flush_fn(conn);
    s->send_state = QUIC_STREAM_SEND_RESET_SENT;

    /* 通知所有 pending write 失败 */
    req_fail_all(s, -(int)error_code);
    return 0;
}

int quic_stream_stop_sending(QuicStreamCtx *ctx, void *conn, uint64_t stream_id,
                              uint64_t error_code,
                              int (*queue_fn)(void *conn, const uint8_t *frames, size_t flen),
                              void (*flush_fn)(void *conn)) {
    QuicStream *s = stream_find(ctx, stream_id);
    if (!s) return -1;

    uint8_t frame[64];
    int flen = quic_frame_write_stop_sending(frame, sizeof(frame),
                                             stream_id, error_code);
    if (flen < 0) return -1;

    queue_fn(conn, frame, (size_t)flen);
    flush_fn(conn);
    s->recv_state = QUIC_STREAM_RECV_RESET_RECVD;
    free(s->recv_buf);
    s->recv_buf = NULL;
    s->recv_buf_len = 0;
    return 0;
}

/* ============================================
 * OOO gap 管理
 * ============================================ */

static void gap_insert(QuicStream *s, uint64_t offset, uint64_t length) {
    int i;
    for (i = 0; i < s->gap_count; i++)
        if (offset < s->gaps[i].offset) break;
    if (s->gap_count < QUIC_STREAM_MAX_GAPS) {
        memmove(&s->gaps[i + 1], &s->gaps[i],
                (size_t)(s->gap_count - i) * sizeof(QuicStreamGap));
        s->gaps[i].offset = offset;
        s->gaps[i].length = length;
        s->gap_count++;
    }
    for (int j = 0; j < s->gap_count - 1; ) {
        uint64_t end_j = s->gaps[j].offset + s->gaps[j].length;
        if (end_j >= s->gaps[j + 1].offset) {
            uint64_t end_next = s->gaps[j + 1].offset + s->gaps[j + 1].length;
            if (end_next > end_j)
                s->gaps[j].length = end_next - s->gaps[j].offset;
            memmove(&s->gaps[j + 1], &s->gaps[j + 2],
                    (size_t)(s->gap_count - j - 2) * sizeof(QuicStreamGap));
            s->gap_count--;
        } else { j++; }
    }
}

static void gap_drain(QuicStream *s, QuicStreamCtx *ctx, void *conn,
                       int (*queue_fn)(void *conn, const uint8_t *frames, size_t flen),
                       void (*flush_fn)(void *conn)) {
    while (s->gap_count > 0 && s->gaps[0].offset <= s->recv_offset) {
        uint64_t gap_end = s->gaps[0].offset + s->gaps[0].length;
        if (gap_end > s->recv_offset) {
            uint64_t new_bytes = gap_end - s->recv_offset;
            uint64_t deliver_off = s->recv_offset;
            s->recv_offset = gap_end;
            ctx->bytes_recvd += new_bytes;
            if (ctx->on_stream_data && s->recv_buf) {
                ctx->on_stream_data((QuicConnection*)conn, s->stream_id,
                                    s->recv_buf + deliver_off, new_bytes, 0);
            }
        }
        memmove(&s->gaps[0], &s->gaps[1],
                (size_t)(s->gap_count - 1) * sizeof(QuicStreamGap));
        s->gap_count--;
    }
}

static void stream_fc_update(QuicStreamCtx *ctx, void *conn, QuicStream *s,
                              int (*queue_fn)(void *conn, const uint8_t *frames, size_t flen)) {
    if (ctx->max_data_local - ctx->bytes_recvd < QUIC_STREAM_RECV_BUF_SIZE / 4) {
        ctx->max_data_local = ctx->bytes_recvd + QUIC_STREAM_RECV_BUF_SIZE;
        uint8_t frm[16];
        int flen = quic_frame_write_max_data(frm, sizeof(frm), ctx->max_data_local);
        if (flen > 0) queue_fn(conn, frm, (size_t)flen);
    }
    if (s->recv_max_data - s->recv_offset < QUIC_STREAM_RECV_BUF_SIZE / 4) {
        s->recv_max_data = s->recv_offset + QUIC_STREAM_RECV_BUF_SIZE;
        uint8_t frm[32];
        int flen = quic_frame_write_max_stream_data(frm, sizeof(frm),
                                                    s->stream_id, s->recv_max_data);
        if (flen > 0) queue_fn(conn, frm, (size_t)flen);
    }
}

/* ============================================
 * 入站 STREAM 帧处理
 * ============================================ */

int quic_stream_on_recv(QuicStreamCtx *ctx, void *conn,
                         uint64_t stream_id, uint64_t offset, int fin,
                         const uint8_t *data, size_t len,
                         int (*queue_fn)(void *conn, const uint8_t *frames, size_t flen),
                         void (*flush_fn)(void *conn)) {
    QuicStream *s = stream_find_or_create(ctx, stream_id);
    if (!s) return -1;

    if (offset + len > s->recv_max_data) {
        LOG_ERROR("[quic-stream] recv flow control exceeded on stream %llu",
                  (unsigned long long)stream_id);
        return -1;
    }

    if (fin) { s->fin_recvd = 1; s->final_size = offset + len; }

    if (offset > s->recv_offset) {
        if (!s->recv_buf) {
            s->recv_buf_cap = (size_t)(offset + len + 65536);
            s->recv_buf = (uint8_t*)malloc(s->recv_buf_cap);
            if (!s->recv_buf) return -1;
        } else if (offset + len > s->recv_buf_cap) {
            size_t new_cap = (size_t)(offset + len + 65536);
            uint8_t *nb = (uint8_t*)realloc(s->recv_buf, new_cap);
            if (!nb) return -1;
            s->recv_buf = nb;
            s->recv_buf_cap = new_cap;
        }
        memcpy(s->recv_buf + offset, data, len);
        gap_insert(s, offset, len);
        ctx->bytes_recvd += len;
        if (offset + len > s->recv_buf_len)
            s->recv_buf_len = offset + len;
        stream_fc_update(ctx, conn, s, queue_fn);
        flush_fn(conn);
        return 0;
    }

    if (offset + len > s->recv_offset) {
        uint64_t new_bytes = offset + len - s->recv_offset;
        uint64_t skip = s->recv_offset - offset;
        ctx->bytes_recvd += new_bytes;
        s->recv_offset = offset + len;
        if (s->recv_offset > s->recv_buf_len)
            s->recv_buf_len = s->recv_offset;
        if (ctx->on_stream_data)
            ctx->on_stream_data((QuicConnection*)conn, stream_id,
                                data + skip, new_bytes, 0);
        gap_drain(s, ctx, conn, queue_fn, flush_fn);
    }

    stream_fc_update(ctx, conn, s, queue_fn);

    if (s->fin_recvd && s->recv_offset >= s->final_size) {
        s->recv_state = QUIC_STREAM_RECV_SIZE_KNOWN;
        if (ctx->on_stream_data)
            ctx->on_stream_data((QuicConnection*)conn, stream_id, NULL, 0, 1);
    }
    flush_fn(conn);
    return 0;
}

/* ============================================
 * 握手完成 / 流控 / RESET / BLOCKED
 * ============================================ */

void quic_stream_handshake_done(QuicStreamCtx *ctx, void *conn,
                                 int (*queue_fn)(void *conn, const uint8_t *frames, size_t flen)) {
    ctx->max_data_local = QUIC_STREAM_RECV_BUF_SIZE;
    /* 不要抬高 max_data_peer：那是对端允许我们发送的额度 */
    uint8_t frame[16];
    int flen = quic_frame_write_max_data(frame, sizeof(frame), ctx->max_data_local);
    if (flen > 0) queue_fn(conn, frame, (size_t)flen);
}

void quic_stream_on_max_data(QuicStreamCtx *ctx, uint64_t max_data) {
    if (max_data > ctx->max_data_peer) ctx->max_data_peer = max_data;
}

void quic_stream_apply_init_send_credit(QuicStreamCtx *ctx) {
    for (size_t i = 0; i < ctx->stream_cnt; i++) {
        QuicStream *s = ctx->streams[i];
        if (!s) continue;
        uint64_t lim = peer_send_credit(ctx, s->stream_id);
        if (lim > s->send_max_data) s->send_max_data = lim;
    }
}

void quic_stream_on_max_stream_data(QuicStreamCtx *ctx,
                                     uint64_t stream_id, uint64_t max_data) {
    QuicStream *s = stream_find(ctx, stream_id);
    if (!s) { s = stream_find_or_create(ctx, stream_id); if (!s) return; }
    if (max_data > s->send_max_data) {
        LOG_DEBUG("[quic-stream] MAX_STREAM_DATA id=%llu %llu -> %llu (off=%llu pending=%zu)",
                 (unsigned long long)stream_id,
                 (unsigned long long)s->send_max_data,
                 (unsigned long long)max_data,
                 (unsigned long long)s->send_offset,
                 s->send_buf_len);
        s->send_max_data = max_data;
    }
}

void quic_stream_on_max_streams(QuicStreamCtx *ctx, uint64_t max_streams, int bidi) {
    if (bidi) ctx->max_streams_bidi_peer = max_streams;
    else      ctx->max_streams_uni_peer  = max_streams;
}

void quic_stream_on_reset(QuicStreamCtx *ctx, uint64_t stream_id,
                           uint64_t final_size) {
    QuicStream *s = stream_find(ctx, stream_id);
    if (!s) return;
    s->recv_state = QUIC_STREAM_RECV_RESET_RECVD;
    s->recv_offset = final_size;
    free(s->recv_buf); s->recv_buf = NULL; s->recv_buf_len = 0;
}

void quic_stream_on_stop_sending(QuicStreamCtx *ctx, void *conn, uint64_t stream_id,
                                  int (*queue_fn)(void *conn, const uint8_t *frames, size_t flen),
                                  void (*flush_fn)(void *conn)) {
    QuicStream *s = stream_find(ctx, stream_id);
    if (!s) return;
    if (s->send_state == QUIC_STREAM_SEND_SEND ||
        s->send_state == QUIC_STREAM_SEND_DATA_SENT) {
        quic_stream_reset(ctx, conn, stream_id, 0, queue_fn, flush_fn);
    }
}

void quic_stream_on_data_blocked(QuicStreamCtx *ctx, uint64_t max_data) {
    (void)ctx; (void)max_data;
}
void quic_stream_on_stream_data_blocked(QuicStreamCtx *ctx,
                                         uint64_t stream_id, uint64_t limit) {
    (void)ctx; (void)stream_id; (void)limit;
}
void quic_stream_on_streams_blocked(QuicStreamCtx *ctx,
                                     uint64_t max_streams, int bidi) {
    (void)ctx; (void)max_streams; (void)bidi;
}

/* ============================================
 * ACK 到达 → 重试被 CWND 阻塞的 stream
 * ============================================ */

void quic_stream_flush_pending(QuicStreamCtx *ctx, void *conn,
                                int (*queue_fn)(void *conn, const uint8_t *frames, size_t flen),
                                void (*flush_fn)(void *conn)) {
    if (ctx->flushing_) return;
    ctx->flushing_ = 1;
    uint64_t now = uv_now(uv_default_loop());
    LOG_DEBUG("[quic-stream] flush_pending: %zu streams", ctx->stream_cnt);
    for (size_t i = 0; i < ctx->stream_cnt; i++) {
        QuicStream *s = ctx->streams[i];
        if (!s) continue;
        /* timeout scan — fire callbacks for expired write_reqs */
        while (s->write_reqs && s->write_reqs->deadline_ms > 0
               && s->write_reqs->deadline_ms <= now) {
            quic_stream_write_req_t *r = s->write_reqs;
            s->write_reqs = r->next;
            if (r->cb) r->cb(s, -2, r->length, r->user_data);
            free(r);
        }
        /* retry pending send_buf（流控/CWND 解开后；不限于 send_blocked） */
        if (s->send_buf_len > 0) {
            LOG_DEBUG("[quic-stream] retry stream %llu: %zu pending",
                      (unsigned long long)s->stream_id, s->send_buf_len);
            consume_send_buf(ctx, conn, s, queue_fn, flush_fn);
        } else {
            maybe_notify_writable(s);
        }
        /* ACK arrived — fire write callbacks for completed reqs */
        if (s->write_reqs) {
            LOG_DEBUG("[quic-stream] flush stream %llu: reqs=%p off=%llu head=(%llu+%llu)",
                     (unsigned long long)s->stream_id, (void*)s->write_reqs,
                     (unsigned long long)s->send_offset,
                     (unsigned long long)s->write_reqs->start_offset,
                     (unsigned long long)s->write_reqs->length);
        }
        req_fire_completed(s, s->send_offset);
        maybe_notify_writable(s);
    }
    ctx->flushing_ = 0;
}

int quic_stream_set_on_writable(QuicStreamCtx *ctx, uint64_t stream_id,
                                 void (*cb)(QuicStream *s, void *user), void *user) {
    QuicStream *s = stream_find(ctx, stream_id);
    if (!s) return -1;
    s->on_writable = cb;
    s->on_writable_user = user;
    return 0;
}

size_t quic_stream_send_pending(QuicStreamCtx *ctx, uint64_t stream_id) {
    QuicStream *s = stream_find(ctx, stream_id);
    return s ? s->send_buf_len : 0;
}

/* ============================================
 * 清理
 * ============================================ */

void quic_stream_cleanup(QuicStreamCtx *ctx) {
    for (size_t i = 0; i < ctx->stream_cnt; i++) {
        QuicStream *s = ctx->streams[i];
        if (s) {
            req_fail_all(s, -1);   /* connection close → fail all pending writes */
            free(s->recv_buf);
            free(s->send_buf);
            free(s);
        }
    }
    free(ctx->streams);
    ctx->streams = NULL;
    ctx->stream_cnt = 0;
    ctx->stream_cap = 0;
}
