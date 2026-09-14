#ifndef HTTP3_QPACK_H
#define HTTP3_QPACK_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================
 * QPACK 静态表 (RFC 9204 §A) — 仅保留 name-string
 * ============================================ */

/* 返回静态表索引对应的 header name / value，未知索引返回 NULL */
const char* qpack_static_name(uint64_t index);
const char* qpack_static_value(uint64_t index);

/* ============================================
 * 解码结果
 * ============================================ */

#define QPACK_MAX_FIELDS 16

typedef struct {
    const uint8_t *name;        /* 指向原始数据的指针 */
    size_t         name_len;
    const uint8_t *value;
    size_t         value_len;
} QpackHeaderField;

/* ============================================
 * 解码 API
 * ============================================ */

/* 解析 QPACK 编码的 HEADERS payload。
 * 返回解析出的字段数，失败返回 -1。
 * 支持：Literal Header Field With Name Reference
 *       (static table name, literal value, no Huffman)
 * 不支持：动态表、Huffman、Indexed Field Line 等。 */
int qpack_decode(const uint8_t *data, size_t len,
                 QpackHeaderField *fields, int max_fields);

#ifdef __cplusplus
}
#endif

#endif /* HTTP3_QPACK_H */
