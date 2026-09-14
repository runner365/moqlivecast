#ifndef HTTP3_FRAME_H
#define HTTP3_FRAME_H

#include "http3_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── 帧编解码 ─────────────────────────────── */

/* DATA 帧: type(1) + length(v) + payload */
int h3_frame_write_data(uint8_t *buf, size_t cap,
                         const uint8_t *data, size_t len);
int h3_frame_parse_data(const uint8_t *buf, size_t len,
                         const uint8_t **data, size_t *data_len);

/* HEADERS 帧: type(1) + length(v) + payload */
int h3_frame_write_headers(uint8_t *buf, size_t cap,
                            const uint8_t *data, size_t len);
int h3_frame_parse_headers(const uint8_t *buf, size_t len,
                            const uint8_t **data, size_t *data_len);

/* SETTINGS 帧: type(1) + length(v) + pairs */
/* (SETTINGS identifiers are defined in http3_common.h) */
int h3_frame_write_settings(uint8_t *buf, size_t cap,
                             const uint64_t *ids, const uint64_t *vals,
                             int count);
int h3_frame_parse_settings(const uint8_t *buf, size_t len,
                             uint64_t *ids, uint64_t *vals,
                             int max_pairs, int *count);

/* GOAWAY 帧: type(1) + length(v) + stream_id(v) */
int h3_frame_write_goaway(uint8_t *buf, size_t cap, uint64_t last_stream_id);
int h3_frame_parse_goaway(const uint8_t *buf, size_t len, uint64_t *last_stream_id);

/* ── 帧类型猜测 ─────────────────────────── */

int h3_frame_type(const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* HTTP3_FRAME_H */
