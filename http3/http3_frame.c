#include "http3_frame.h"
#include "qpack.h"
#include <string.h>
#include <stdio.h>

/* ============================================
 * DATA (0x00)
 * ============================================ */

int h3_frame_write_data(uint8_t *buf, size_t cap,
                         const uint8_t *data, size_t len) {
    size_t llen = quic_varint_len((uint64_t)len);
    if (1 + llen + len > cap) return -1;
    size_t pos = 0;
    buf[pos++] = H3_FRAME_DATA;
    pos += quic_varint_encode(buf + pos, cap - pos, (uint64_t)len);
    if (len > 0) memcpy(buf + pos, data, len);
    pos += len;
    return (int)pos;
}

int h3_frame_parse_data(const uint8_t *buf, size_t len,
                         const uint8_t **data, size_t *data_len) {
    if (len < 1 || buf[0] != H3_FRAME_DATA) return -1;
    uint64_t vlen;
    int v = quic_varint_decode(buf + 1, len - 1, &vlen);
    if (v < 0) return -1;
    size_t pos = 1 + v;
    if (pos + vlen > len) return -1;
    *data = buf + pos;
    *data_len = (size_t)vlen;
    return (int)(pos + vlen);
}

/* ============================================
 * HEADERS (0x01)
 * ============================================ */

int h3_frame_write_headers(uint8_t *buf, size_t cap,
                            const uint8_t *data, size_t len) {
    size_t llen = quic_varint_len((uint64_t)len);
    if (1 + llen + len > cap) return -1;
    size_t pos = 0;
    buf[pos++] = H3_FRAME_HEADERS;
    pos += quic_varint_encode(buf + pos, cap - pos, (uint64_t)len);
    if (len > 0) memcpy(buf + pos, data, len);
    pos += len;
    return (int)pos;
}

int h3_frame_parse_headers(const uint8_t *buf, size_t len,
                            const uint8_t **data, size_t *data_len) {
    if (len < 1 || buf[0] != H3_FRAME_HEADERS) return -1;
    uint64_t vlen;
    int v = quic_varint_decode(buf + 1, len - 1, &vlen);
    if (v < 0) return -1;
    size_t pos = 1 + v;
    if (pos + vlen > len) return -1;
    *data = buf + pos;
    *data_len = (size_t)vlen;
    return (int)(pos + vlen);
}

/* ============================================
 * SETTINGS (0x04)
 * ============================================ */

int h3_frame_write_settings(uint8_t *buf, size_t cap,
                             const uint64_t *ids, const uint64_t *vals,
                             int count) {
    /* 预计算 payload 大小 */
    size_t payload = 0;
    for (int i = 0; i < count; i++)
        payload += quic_varint_len(ids[i]) + quic_varint_len(vals[i]);

    size_t llen = quic_varint_len((uint64_t)payload);
    if (1 + llen + payload > cap) return -1;

    size_t pos = 0;
    buf[pos++] = H3_FRAME_SETTINGS;
    pos += quic_varint_encode(buf + pos, cap - pos, (uint64_t)payload);
    for (int i = 0; i < count; i++) {
        pos += quic_varint_encode(buf + pos, cap - pos, ids[i]);
        pos += quic_varint_encode(buf + pos, cap - pos, vals[i]);
    }
    return (int)pos;
}

int h3_frame_parse_settings(const uint8_t *buf, size_t len,
                             uint64_t *ids, uint64_t *vals,
                             int max_pairs, int *count) {
    if (len < 1 || buf[0] != H3_FRAME_SETTINGS) return -1;

    uint64_t plen;
    int v = quic_varint_decode(buf + 1, len - 1, &plen);
    if (v < 0) return -1;
    size_t pos = 1 + v;
    size_t pend = pos + plen;
    if (pend > len) return -1;

    *count = 0;
    while (pos < pend && *count < max_pairs) {
        uint64_t sid, sval;
        int vi = quic_varint_decode(buf + pos, pend - pos, &sid);
        if (vi < 0) break;
        pos += vi;
        int vv = quic_varint_decode(buf + pos, pend - pos, &sval);
        if (vv < 0) break;
        pos += vv;
        ids[*count] = sid;
        vals[*count] = sval;
        (*count)++;
    }
    return (int)pend;
}

/* ============================================
 * 帧类型
 * ============================================ */

int h3_frame_type(const uint8_t *data, size_t len) {
    if (len < 1) return -1;
    /* SETTINGS 帧只在控制流出现，任何 bidi 上的第一帧不会是 SETTINGS */
    return (int)data[0];
}

/* ============================================
 * 简易 header 解析/构造（纯文本）
 * ============================================ */

int h3_parse_request_headers(const uint8_t *data, size_t len,
                              const char **method, const char **path) {
    *method = NULL;
    *path = NULL;

    /* QPACK 解码 */
    QpackHeaderField fields[QPACK_MAX_FIELDS];
    int n = qpack_decode(data, len, fields, QPACK_MAX_FIELDS);

    if (n <= 0) {
        /* fallback: plain text parse */
        char hdr[4096];
        size_t bn = len < sizeof(hdr) - 1 ? len : sizeof(hdr) - 1;
        memcpy(hdr, data, bn);
        hdr[bn] = '\0';
        char *start = hdr;
        while (start && *start) {
            char *end = strstr(start, "\r\n");
            if (!end) break;
            *end = '\0';
            if (strncmp(start, ":method ", 8) == 0)
                *method = (const char*)data + (start + 8 - hdr);
            else if (strncmp(start, ":path ", 6) == 0)
                *path   = (const char*)data + (start + 6 - hdr);
            start = end + 2;
        }
        return 0;
    }

    /* 遍历 QPACK 解码的字段，匹配 :method 和 :path */
    for (int i = 0; i < n; i++) {
        if (!fields[i].name) continue;
        if (fields[i].name_len == 7 &&
            memcmp(fields[i].name, ":method", 7) == 0) {
            /* Indexed Header Field (value_len==0) → 使用静态表默认值 */
            *method = (fields[i].value && fields[i].value_len > 0)
                    ? (const char*)fields[i].value : "GET";
        } else if (fields[i].name_len == 5 &&
                   memcmp(fields[i].name, ":path", 5) == 0) {
            *path   = (const char*)fields[i].value;
        }
    }

    /* fallback — if QPACK didn't find :method, default to GET */
    if (!*method) *method = "GET";

    return 0;
}

int h3_format_response_headers(uint8_t *buf, size_t cap,
                                int status, const char *status_text,
                                const char *extra_headers) {
    return snprintf((char*)buf, cap,
                    ":status %d\r\n%s%s"
                    "\r\n",
                    status,
                    extra_headers ? extra_headers : "",
                    extra_headers ? "\r\n" : "");
}

/* ============================================
 * GOAWAY (0x07)
 * ============================================ */

int h3_frame_write_goaway(uint8_t *buf, size_t cap, uint64_t last_stream_id) {
    size_t id_len = quic_varint_len(last_stream_id);
    size_t payload = id_len;
    size_t pl_len  = quic_varint_len((uint64_t)payload);
    if (1 + pl_len + payload > cap) return -1;
    size_t pos = 0;
    buf[pos++] = H3_FRAME_GOAWAY;
    pos += quic_varint_encode(buf + pos, cap - pos, (uint64_t)payload);
    quic_varint_encode(buf + pos, cap - pos, last_stream_id);
    return (int)(1 + pl_len + payload);
}

int h3_frame_parse_goaway(const uint8_t *buf, size_t len,
                           uint64_t *last_stream_id) {
    if (len < 1 || buf[0] != H3_FRAME_GOAWAY) return -1;
    uint64_t plen;
    int v = quic_varint_decode(buf + 1, len - 1, &plen);
    if (v < 0) return -1;
    size_t pos = 1 + v;
    if (pos + plen > len) return -1;
    v = quic_varint_decode(buf + pos, len - pos, last_stream_id);
    if (v < 0) return -1;
    return (int)(pos + plen);
}
