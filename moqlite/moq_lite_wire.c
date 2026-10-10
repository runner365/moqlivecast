#include "moq_lite_wire.h"

#include <stdlib.h>
#include <string.h>

/* ============================================================
 * 可增长缓冲
 * ============================================================ */

void moq_buf_init(MoqBuf *b) {
    if (!b) return;
    b->buf = NULL;
    b->len = 0;
    b->cap = 0;
}

void moq_buf_free(MoqBuf *b) {
    if (!b) return;
    free(b->buf);
    b->buf = NULL;
    b->len = 0;
    b->cap = 0;
}

void moq_buf_reset(MoqBuf *b) { moq_buf_free(b); }

void moq_buf_clear(MoqBuf *b) {
    if (b) b->len = 0;
}

int moq_buf_reserve(MoqBuf *b, size_t extra) {
    if (!b) return -1;
    if (b->len + extra <= b->cap) return 0;

    /* 翻倍增长，但至少装下这次的请求。起步 64 字节 —— 控制消息都很小，
     * 而 GROUP/FRAME 的编码缓冲是复用的，涨上去就不再回落。 */
    size_t want = b->cap ? b->cap : 64;
    while (want < b->len + extra) {
        if (want > (SIZE_MAX / 2)) return -1;   /* 溢出守卫 */
        want *= 2;
    }

    uint8_t *nb = (uint8_t*)realloc(b->buf, want);
    if (!nb) return -1;
    b->buf = nb;
    b->cap = want;
    return 0;
}

int moq_buf_append(MoqBuf *b, const void *p, size_t n) {
    if (!b) return -1;
    if (n == 0) return 0;
    if (!p) return -1;
    if (moq_buf_reserve(b, n) < 0) return -1;
    memcpy(b->buf + b->len, p, n);
    b->len += n;
    return 0;
}

/* ============================================================
 * (i) —— QUIC varint (RFC 9000 §16)
 * ============================================================ */

size_t moq_varint_size(uint64_t v) {
    if (v <= 63)          return 1;
    if (v <= 16383)       return 2;
    if (v <= 1073741823)  return 4;   /* 2^30 - 1 */
    return 8;
}

int moq_varint_write(MoqBuf *b, uint64_t v) {
    if (!b) return -1;
    /* ⚠️ 先判上限 —— 见头文件的说明：不判就是静默截断成另一个数。 */
    if (v > MOQ_VARINT_MAX) return -1;

    uint8_t tmp[8];
    const size_t n = moq_varint_size(v);
    switch (n) {
    case 1:
        tmp[0] = (uint8_t)v;
        break;
    case 2:
        tmp[0] = (uint8_t)(0x40 | (v >> 8));
        tmp[1] = (uint8_t)v;
        break;
    case 4:
        tmp[0] = (uint8_t)(0x80 | (v >> 24));
        tmp[1] = (uint8_t)(v >> 16);
        tmp[2] = (uint8_t)(v >> 8);
        tmp[3] = (uint8_t)v;
        break;
    default:
        tmp[0] = (uint8_t)(0xc0 | (v >> 56));
        tmp[1] = (uint8_t)(v >> 48);
        tmp[2] = (uint8_t)(v >> 40);
        tmp[3] = (uint8_t)(v >> 32);
        tmp[4] = (uint8_t)(v >> 24);
        tmp[5] = (uint8_t)(v >> 16);
        tmp[6] = (uint8_t)(v >> 8);
        tmp[7] = (uint8_t)v;
        break;
    }
    return moq_buf_append(b, tmp, n);
}

int moq_varint_read(const uint8_t *p, size_t len, size_t *off, uint64_t *out) {
    if (!off) return -2;
    if (*off >= len) return -1;          /* 空缓冲是流式解析的常态，不是错误 */

    const uint8_t first = p[*off];
    const size_t  n     = (size_t)1 << (first >> 6);   /* 1 / 2 / 4 / 8 */
    if (len - *off < n) return -1;

    uint64_t v = first & 0x3f;
    for (size_t i = 1; i < n; i++) v = (v << 8) | p[*off + i];

    if (out) *out = v;
    *off += n;
    return 0;
}

int moq_varint_skip(const uint8_t *p, size_t len, size_t *off) {
    return moq_varint_read(p, len, off, NULL);
}

/* ============================================================
 * zigzag
 * ============================================================ */

int moq_zigzag_encode(int64_t v, uint64_t *out) {
    if (v < MOQ_ZIGZAG_MIN || v > MOQ_ZIGZAG_MAX) return -1;
    if (out) {
        /* 先转无符号再左移，避免有符号左移溢出（UB）；>> 63 要的是算术右移
         * （负数全 1），gcc/clang 都是。 */
        *out = ((uint64_t)v << 1) ^ (uint64_t)(v >> 63);
    }
    return 0;
}

int64_t moq_zigzag_decode(uint64_t v) {
    return (int64_t)(v >> 1) ^ -(int64_t)(v & 1);
}

/* ============================================================
 * (s) —— 长度前缀串
 * ============================================================ */

int moq_str_write_n(MoqBuf *b, const void *p, size_t n) {
    if (moq_varint_write(b, n) < 0) return -1;
    return moq_buf_append(b, p, n);
}

int moq_str_write(MoqBuf *b, const char *s) {
    if (!s) return moq_str_write_n(b, NULL, 0);
    return moq_str_write_n(b, s, strlen(s));
}

int moq_str_read(const uint8_t *p, size_t end, size_t *off, MoqStr *out) {
    uint64_t n = 0;
    if (moq_varint_read(p, end, off, &n) < 0) return -1;
    /* 声明长度超过剩余字节 —— 这不是「慢慢等能等到」，流式下必须与「不够」
     * 分开：等待只会在下一次回调再判一次，永远等不到。 */
    if (n > end - *off) return -2;
    if (out) {
        out->p   = p + *off;
        out->len = (size_t)n;
    }
    *off += (size_t)n;
    return 0;
}

char *moq_str_dup(const MoqStr *s) {
    if (!s) return NULL;
    char *c = (char*)malloc(s->len + 1);
    if (!c) return NULL;
    if (s->len) memcpy(c, s->p, s->len);
    c[s->len] = '\0';
    return c;
}

/* ============================================================
 * 消息框
 * ============================================================ */

MoqMsgStatus moq_msg_read(const uint8_t *p, size_t len, size_t *off,
                          size_t *body_len) {
    size_t o = off ? *off : 0;
    uint64_t n = 0;
    if (moq_varint_read(p, len, &o, &n) < 0) return MOQ_MSG_NEED_MORE;
    if (n > MOQ_MAX_MESSAGE_SIZE) return MOQ_MSG_TOO_LARGE;
    if (n > len - o) return MOQ_MSG_NEED_MORE;   /* o <= len 由上一步保证 */
    if (body_len) *body_len = (size_t)n;
    if (off) *off = o;
    return MOQ_MSG_OK;
}

int moq_msg_build(MoqBuf *out, int typed, uint64_t type,
                  const void *body, size_t body_len) {
    if (typed) {
        if (moq_varint_write(out, type) < 0) return -1;
    }
    if (moq_varint_write(out, body_len) < 0) return -1;
    return moq_buf_append(out, body, body_len);
}

int moq_put_stream_type(MoqBuf *out, uint64_t stream_type) {
    return moq_varint_write(out, stream_type);
}

/* ============================================================
 * 发布端（serve）方向
 * ============================================================ */

int moq_wire_announce_ok(MoqBuf *out, uint64_t origin, uint64_t active) {
    MoqBuf body; moq_buf_init(&body);
    int rc = moq_varint_write(&body, origin);
    if (rc == 0) rc = moq_varint_write(&body, active);
    /* typed=0：ANNOUNCE_OK 是普通消息，线上没有类型判别符 */
    if (rc == 0) rc = moq_msg_build(out, 0, 0, body.buf, body.len);
    moq_buf_free(&body);
    return rc;
}

int moq_wire_announce_start(MoqBuf *out, const char *suffix,
                            uint64_t warm, uint64_t cold) {
    MoqBuf body; moq_buf_init(&body);
    int rc = moq_str_write(&body, suffix);
    /* hops count = 0：本端是内容源头，既没有上游也没有跳数可报。 */
    if (rc == 0) rc = moq_varint_write(&body, 0);
    if (rc == 0) rc = moq_varint_write(&body, warm);
    if (rc == 0) rc = moq_varint_write(&body, cold);
    if (rc == 0) rc = moq_msg_build(out, 1, /*ANNOUNCE_START=*/0x0,
                                    body.buf, body.len);
    moq_buf_free(&body);
    return rc;
}

int moq_wire_subscribe_start(MoqBuf *out, uint64_t group) {
    MoqBuf body; moq_buf_init(&body);
    int rc = moq_varint_write(&body, group);
    if (rc == 0) rc = moq_msg_build(out, 1, /*SUBSCRIBE_START=*/0x0,
                                    body.buf, body.len);
    moq_buf_free(&body);
    return rc;
}

int moq_wire_subscribe_end(MoqBuf *out, uint64_t group) {
    MoqBuf body; moq_buf_init(&body);
    int rc = moq_varint_write(&body, group);
    if (rc == 0) rc = moq_msg_build(out, 1, /*SUBSCRIBE_END=*/0x1,
                                    body.buf, body.len);
    moq_buf_free(&body);
    return rc;
}

int moq_wire_track_info(MoqBuf *out, uint8_t priority,
                        uint64_t max_age_ms, uint64_t timescale) {
    MoqBuf body; moq_buf_init(&body);
    int rc = moq_buf_append(&body, &priority, 1);   /* priority 是裸 u8 */
    if (rc == 0) rc = moq_varint_write(&body, max_age_ms);
    if (rc == 0) rc = moq_varint_write(&body, timescale);
    /* typed=0：TRACK_INFO 是普通消息 */
    if (rc == 0) rc = moq_msg_build(out, 0, 0, body.buf, body.len);
    moq_buf_free(&body);
    return rc;
}

int moq_wire_group_header(MoqBuf *out, uint64_t sub_id, uint64_t seq,
                          uint64_t frame_start) {
    MoqBuf body; moq_buf_init(&body);
    int rc = moq_varint_write(&body, sub_id);
    if (rc == 0) rc = moq_varint_write(&body, seq);
    if (rc == 0) rc = moq_varint_write(&body, frame_start);
    if (rc == 0) rc = moq_msg_build(out, 0, 0, body.buf, body.len);
    moq_buf_free(&body);
    return rc;
}

int moq_wire_frame(MoqBuf *out, int64_t ts_delta, const void *payload,
                   size_t len) {
    uint64_t zz = 0;
    if (moq_zigzag_encode(ts_delta, &zz) < 0) return -1;
    if (moq_varint_write(out, zz) < 0) return -1;
    /* Message Length 只覆盖 payload（不含前面那个差），所以不走 moq_msg_build
     * —— 那个会先写类型再写长度，语义不对。 */
    if (moq_varint_write(out, len) < 0) return -1;
    return moq_buf_append(out, payload, len);
}
