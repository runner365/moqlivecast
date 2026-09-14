#ifndef HTTP3_HUFFMAN_H
#define HTTP3_HUFFMAN_H
#include <stdint.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif

/* State and flags from nghttp3 */
#define HUFFMAN_FLAG_ACCEPTED 0x01U
#define HUFFMAN_FLAG_SYM      0x02U

typedef struct {
  uint16_t fstate; uint8_t flags; uint8_t sym;
} huffman_decode_node;

typedef struct {
  uint16_t fstate; uint8_t flags;
} huffman_decode_ctx;

extern const huffman_decode_node huffman_decode_table[][16];

void    huffman_decode_init(huffman_decode_ctx *ctx);
int     huffman_decode_str(huffman_decode_ctx *ctx, uint8_t *dest,
                           const uint8_t *src, size_t srclen, int fin);
int     huffman_decode_failure(const huffman_decode_ctx *ctx);

#ifdef __cplusplus
}
#endif
#endif
