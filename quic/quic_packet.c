#include "quic_packet.h"
#include "quic_crypto.h"
#include "logger.h"
#include <string.h>
#include <stdio.h>
#include <arpa/inet.h>

/* ============================================
 * Varint (RFC 9000 §16)
 * ============================================ */

int quic_varint_encode(uint8_t *buf, size_t cap, uint64_t val) {
    size_t len = quic_varint_len(val);
    if (cap < len) {
        LOG_ERROR("[quic-packet] varint_encode: cap=%zu < needed=%zu val=%llu", cap, len,
                  (unsigned long long)val);
        return -1;
    }

    switch (len) {
    case 1: buf[0] = (uint8_t)val; break;
    case 2: buf[0] = (uint8_t)(val >> 8) | 0x40;
            buf[1] = (uint8_t)val; break;
    case 4: buf[0] = (uint8_t)(val >> 24) | 0x80;
            buf[1] = (uint8_t)(val >> 16);
            buf[2] = (uint8_t)(val >> 8);
            buf[3] = (uint8_t)val; break;
    case 8: buf[0] = (uint8_t)(val >> 56) | 0xc0;
            buf[1] = (uint8_t)(val >> 48);
            buf[2] = (uint8_t)(val >> 40);
            buf[3] = (uint8_t)(val >> 32);
            buf[4] = (uint8_t)(val >> 24);
            buf[5] = (uint8_t)(val >> 16);
            buf[6] = (uint8_t)(val >> 8);
            buf[7] = (uint8_t)val; break;
    }
    return (int)len;
}

int quic_varint_decode(const uint8_t *data, size_t len, uint64_t *val) {
    if (len < 1) {
        LOG_ERROR("[quic-packet] varint_decode: len=%zu < 1", len);
        return -1;
    }
    uint8_t first = data[0];
    size_t vlen = 1 << ((first >> 6) & 0x3);  /* 1, 2, 4, or 8 */
    if (len < vlen) {
        LOG_ERROR("[quic-packet] varint_decode: len=%zu < vlen=%zu (first=0x%02x)",
                  len, vlen, first);
        return -1;
    }

    switch (vlen) {
    case 1: *val = first; break;
    case 2: *val = ((uint64_t)(first & 0x3f) << 8) | data[1]; break;
    case 4: *val = ((uint64_t)(first & 0x3f) << 24) | ((uint64_t)data[1] << 16)
                  | ((uint64_t)data[2] << 8) | data[3]; break;
    case 8: *val = ((uint64_t)(first & 0x3f) << 56) | ((uint64_t)data[1] << 48)
                  | ((uint64_t)data[2] << 40) | ((uint64_t)data[3] << 32)
                  | ((uint64_t)data[4] << 24) | ((uint64_t)data[5] << 16)
                  | ((uint64_t)data[6] << 8) | data[7]; break;
    default: return -1;
    }
    return (int)vlen;
}

size_t quic_varint_len(uint64_t val) {
    if (val <= 63)        return 1;
    if (val <= 16383)      return 2;
    if (val <= 1073741823) return 4;
    return 8;
}

/* ============================================
 * Packet Number (RFC 9000 §17.1)
 * ============================================ */

size_t quic_pn_encode_len(uint64_t pn) {
    if (pn <= 0xff)        return 1;
    if (pn <= 0xffff)      return 2;
    if (pn <= 0xffffff)    return 3;
    return 4;
}

void quic_pn_encode(uint8_t *buf, uint64_t pn, size_t len) {
    for (size_t i = 0; i < len; i++) {
        buf[len - 1 - i] = (uint8_t)(pn >> (8 * i));
    }
}

uint64_t quic_pn_decode(const uint8_t *buf, size_t len, uint64_t expected) {
    /* RFC 9000 Appendix A.3 */
    uint64_t truncated = 0;
    for (size_t i = 0; i < len; i++) {
        truncated = (truncated << 8) | buf[i];
    }
    unsigned pn_nbits = (unsigned)(8 * len);
    uint64_t pn_win = (uint64_t)1 << pn_nbits;
    uint64_t pn_hwin = pn_win / 2;
    uint64_t candidate = (expected & ~(pn_win - 1)) | truncated;
    if (candidate + pn_hwin <= expected) {
        return candidate + pn_win;
    }
    if (candidate > expected + pn_hwin && candidate > pn_win) {
        return candidate - pn_win;
    }
    return candidate;
}

/* ============================================
 * 长头包 (RFC 9000 §17.2)
 * ============================================ */

int quic_packet_build_long(uint8_t *out, size_t *out_len,
                           int pkt_type,
                           const QuicConnectionId *src_cid,
                           const QuicConnectionId *dst_cid,
                           const uint8_t *token, size_t token_len,
                           uint64_t pn,
                           const uint8_t *payload, size_t payload_len,
                           QuicCipherKeys *keys) {
    size_t pn_len = quic_pn_encode_len(pn);
    if (pn_len > 4) {
        LOG_ERROR("[quic-packet] build_long: pn_len=%zu > 4", pn_len);
        return -1;
    }

    /* 第一字节 */
    uint8_t first_byte = QUIC_LONG_HEADER | QUIC_FIXED_BIT | (uint8_t)(pkt_type << 4)
                       | (uint8_t)((pn_len - 1) & 0x03);

    /* 构造未保护的头部 */
    uint8_t header[256];  /* 足够大 */
    size_t  header_len = 0;
    header[header_len++] = first_byte;

    /* Version (4 字节) */
    uint32_t version_be = htonl(QUIC_VERSION_V1);
    memcpy(header + header_len, &version_be, 4); header_len += 4;

    /* DCID */
    header[header_len++] = dst_cid->len;
    if (dst_cid->len > 0) {
        memcpy(header + header_len, dst_cid->data, dst_cid->len);
        header_len += dst_cid->len;
    }

    /* SCID */
    header[header_len++] = src_cid->len;
    if (src_cid->len > 0) {
        memcpy(header + header_len, src_cid->data, src_cid->len);
        header_len += src_cid->len;
    }

    /* Token (仅 Initial 包) */
    if (pkt_type == QUIC_PKT_INITIAL) {
        int tlen_w = quic_varint_encode(header + header_len, sizeof(header) - header_len, token_len);
        if (tlen_w < 0) {
            LOG_ERROR("[quic-packet] build_long: token varint encode failed, token_len=%zu cap=%zu",
                      token_len, sizeof(header) - header_len);
            return -1;
        }
        header_len += tlen_w;
        if (token_len > 0) {
            if (header_len + token_len > sizeof(header)) {
                LOG_ERROR("[quic-packet] build_long: token too large, header_len=%zu token_len=%zu cap=%zu",
                          header_len, token_len, sizeof(header));
                return -1;
            }
            memcpy(header + header_len, token, token_len);
            header_len += token_len;
        }
    }

    /* Initial 包必须 ≥ 1200 字节 (RFC 9000 §14.1)。
     * 如果总长不足，在 payload 末尾补 PADDING 帧 */
    uint8_t pad_buf[QUIC_MAX_PKT_SIZE];
    if (pkt_type == QUIC_PKT_INITIAL) {
        size_t body_nopad = pn_len + payload_len + 16;
        size_t lv_sz       = quic_varint_len(body_nopad);
        size_t tentative   = header_len + (size_t)lv_sz + body_nopad;
        if (tentative < QUIC_MIN_PKT_SIZE) {
            size_t pad = QUIC_MIN_PKT_SIZE - tentative;
            if (payload_len + pad <= sizeof(pad_buf)) {
                memcpy(pad_buf, payload, payload_len);
                memset(pad_buf + payload_len, 0, pad);
                payload      = pad_buf;
                payload_len += pad;
                LOG_DEBUG("[quic-packet] build_long: padded +%zu bytes → %zu total payload",
                          pad, payload_len);
            }
        }
    }

    /* Length — payload + pn + 16 (tag) */
    size_t body_len = pn_len + payload_len + 16;
    int llen_w = quic_varint_encode(header + header_len, sizeof(header) - header_len, body_len);
    if (llen_w < 0) {
        LOG_ERROR("[quic-packet] build_long: length varint encode failed, body_len=%zu cap=%zu",
                  body_len, sizeof(header) - header_len);
        return -1;
    }
    header_len += llen_w;

    /* AAD = everything from first byte up to and including PN field (RFC 9001 §5.3) */
    size_t aad_len = header_len + pn_len;

    /* 构建 packet: header + pn + payload */
    size_t body_offset = header_len + pn_len;
    size_t total_pkt_len = header_len + body_len;
    if (total_pkt_len > QUIC_MAX_PKT_SIZE) {
        LOG_ERROR("[quic-packet] build_long: total_pkt_len=%zu > QUIC_MAX_PKT_SIZE=%u",
                  total_pkt_len, QUIC_MAX_PKT_SIZE);
        return -1;
    }

    memcpy(out, header, header_len);
    quic_pn_encode(out + header_len, pn, pn_len);
    if (payload_len > 0) {
        memcpy(out + body_offset, payload, payload_len);
    }

    /* AEAD encrypt payload only (after PN). AAD includes up to and including PN. */
    size_t pt_len = payload_len;
    uint8_t *pt_pos = out + body_offset;

    size_t ct_len;
    int ret = quic_crypto_encrypt(keys, pn,
                                  out /* aad */, aad_len,
                                  pt_pos, pt_len,
                                  pt_pos, &ct_len);
    if (ret < 0) {
        LOG_ERROR("[quic-packet] build_long: AEAD encrypt failed, pn=%llu pkt_type=%d aad_len=%zu pt_len=%zu",
                  (unsigned long long)pn, pkt_type, aad_len, pt_len);
        return -1;
    }

    *out_len = header_len + pn_len + ct_len;

    LOG_DEBUG("[quic-packet] build_long: type=%d hdr=%zu pn_len=%zu pl=%zu body_len=%zu"
             " ct_len=%zu out_len=%zu",
             pkt_type, header_len, pn_len, payload_len, body_len,
             ct_len, *out_len);

    /* Header Protection — pn_offset is the position where PN starts */
    size_t pn_off = header_len;
    quic_crypto_hp_apply(keys, out, *out_len, pn_off, pn_len);

    return 0;
}

int quic_packet_parse_long(const uint8_t *data, size_t len,
                           int *pkt_type,
                           QuicConnectionId *src_cid,
                           QuicConnectionId *dst_cid,
                           const uint8_t **token, size_t *token_len,
                           uint64_t *pn,
                           const uint8_t **payload, size_t *payload_len,
                           QuicCipherKeys *keys,
                           uint64_t expected_pn,
                           size_t *consumed) {
    if (len < 7) {
        LOG_ERROR("[quic-packet] parse_long: len=%zu < 7", len);
        return -1;
    }

    if ((data[0] & 0x80) == 0) {
        LOG_ERROR("[quic-packet] parse_long: not a long header, first_byte=0x%02x", data[0]);
        return -1;
    }

    size_t pos = 1; /* 跳过 first byte */

    /* version */
    if (pos + 4 > len) {
        LOG_ERROR("[quic-packet] parse_long: truncated version field, pos=%zu len=%zu", pos, len);
        return -1;
    }
    uint32_t ver_n;
    memcpy(&ver_n, data + pos, 4);
    if (ntohl(ver_n) != QUIC_VERSION_V1) {
        LOG_ERROR("[quic-packet] parse_long: unsupported version 0x%08x (expected 0x%08x)",
                  ntohl(ver_n), QUIC_VERSION_V1);
        return -1;
    }
    pos += 4;

    /* DCID */
    if (pos >= len) {
        LOG_ERROR("[quic-packet] parse_long: truncated after version, pos=%zu len=%zu", pos, len);
        return -1;
    }
    uint8_t dcid_len = data[pos++];
    if (pos + dcid_len > len) {
        LOG_ERROR("[quic-packet] parse_long: DCID overflow, pos=%zu dcid_len=%u len=%zu",
                  pos, dcid_len, len);
        return -1;
    }
    if (dcid_len > 0 && dcid_len <= QUIC_CID_MAX_LEN) {
        memcpy(dst_cid->data, data + pos, dcid_len);
        dst_cid->len = dcid_len;
    } else {
        dst_cid->len = 0;
    }
    pos += dcid_len;

    /* SCID */
    if (pos >= len) {
        LOG_ERROR("[quic-packet] parse_long: truncated after DCID, pos=%zu len=%zu", pos, len);
        return -1;
    }
    uint8_t scid_len = data[pos++];
    if (pos + scid_len > len) {
        LOG_ERROR("[quic-packet] parse_long: SCID overflow, pos=%zu scid_len=%u len=%zu",
                  pos, scid_len, len);
        return -1;
    }
    if (scid_len > 0 && scid_len <= QUIC_CID_MAX_LEN) {
        memcpy(src_cid->data, data + pos, scid_len);
        src_cid->len = scid_len;
    } else {
        src_cid->len = 0;
    }
    pos += scid_len;

    /* token */
    *token = NULL;
    *token_len = 0;
    int raw_type = (data[0] >> 4) & 0x03;
    if (raw_type == QUIC_PKT_INITIAL) {
        if (pos >= len) {
            LOG_ERROR("[quic-packet] parse_long: truncated before token, pos=%zu len=%zu", pos, len);
            return -1;
        }
        uint64_t tok_len;
        int tv = quic_varint_decode(data + pos, len - pos, &tok_len);
        if (tv < 0) {
            LOG_ERROR("[quic-packet] parse_long: token varint decode failed, pos=%zu len=%zu",
                      pos, len);
            return -1;
        }
        pos += tv;
        if (pos + tok_len > len) {
            LOG_ERROR("[quic-packet] parse_long: token overflow, pos=%zu tok_len=%llu len=%zu",
                      pos, (unsigned long long)tok_len, len);
            return -1;
        }
        if (tok_len > 0) {
            *token = data + pos;
            *token_len = (size_t)tok_len;
        }
        pos += (size_t)tok_len;
    }

    /* Length field */
    if (pos >= len) {
        LOG_ERROR("[quic-packet] parse_long: truncated before length field, pos=%zu len=%zu",
                  pos, len);
        return -1;
    }
    uint64_t body_len;
    int lv = quic_varint_decode(data + pos, len - pos, &body_len);
    if (lv < 0) {
        LOG_ERROR("[quic-packet] parse_long: length varint decode failed, pos=%zu len=%zu",
                  pos, len);
        return -1;
    }
    pos += lv;
    size_t pn_offset = pos;

    if (pn_offset + 20 > len) {
        LOG_ERROR("[quic-packet] parse_long: not enough data for HP sample, pn_offset=%zu len=%zu",
                  pn_offset, len);
        return -1;
    }

    LOG_DEBUG("[quic-packet] parse_long: pn_offset=%zu body_len=%llu dcid_len=%u scid_len=%u",
              pn_offset, (unsigned long long)body_len, dcid_len, scid_len);

    /* HP removal — internally handles pn_len chicken-and-egg */
    int pn_len_ret = quic_crypto_hp_remove_long(keys, (uint8_t*)data, len, pn_offset);
    if (pn_len_ret <= 0) {
        LOG_ERROR("[quic-packet] parse_long: HP remove failed, pn_offset=%zu len=%zu",
                  pn_offset, len);
        return -1;
    }
    size_t pn_len = (size_t)pn_len_ret;

    *pn = quic_pn_decode(data + pn_offset, pn_len, expected_pn);

    size_t ct_len_val = (size_t)body_len - pn_len;
    if (pn_offset + pn_len + ct_len_val > len) {
        LOG_ERROR("[quic-packet] parse_long: ciphertext overflow, pn_offset=%zu pn_len=%zu"
                  " ct_len=%zu len=%zu",
                  pn_offset, pn_len, ct_len_val, len);
        return -1;
    }

    /* AEAD decrypt (payload only, PN excluded) */
    size_t ct_pos = pn_offset + pn_len;
    LOG_DEBUG("[quic-packet] parse_long: pn=%llu pn_len=%zu ct_pos=%zu ct_len=%zu aad_len=%zu",
              (unsigned long long)*pn, pn_len, ct_pos, ct_len_val, pn_offset + pn_len);
    /* AAD includes up to and including PN (RFC 9001 §5.3) */
    int ret = quic_crypto_decrypt(keys, *pn,
                                  data /* aad */, pn_offset + pn_len,
                                  data + ct_pos, ct_len_val,
                                  (uint8_t*)(data + ct_pos), payload_len);
    if (ret < 0) {
        LOG_ERROR("[quic-packet] parse_long: AEAD decrypt failed, pn=%llu pn_len=%zu"
                  " ct_pos=%zu ct_len=%zu aad_len=%zu expected_pn=%llu",
                  (unsigned long long)*pn, pn_len, ct_pos, ct_len_val,
                  pn_offset + pn_len, (unsigned long long)expected_pn);
        return -1;
    }

    *payload = data + ct_pos;
    *pkt_type = raw_type;
    *consumed = pn_offset + pn_len + ct_len_val;
    return 0;
}

/* ============================================
 * 短头包 (RFC 9000 §17.3)
 * ============================================ */

int quic_packet_build_short(uint8_t *out, size_t *out_len,
                            const QuicConnectionId *dst_cid,
                            uint64_t pn,
                            const uint8_t *payload, size_t payload_len,
                            QuicCipherKeys *keys,
                            int key_phase,
                            int spin_bit) {
    size_t pn_len = quic_pn_encode_len(pn);

    /* 第一字节: 0 | 1 (fixed) | spin | 00 (reserved) | key_phase | pn_len-1 */
    uint8_t first_byte = QUIC_FIXED_BIT
                       | (uint8_t)((spin_bit  & 0x01) << 5)
                       | (uint8_t)((key_phase & 0x01) << 2)
                       | (uint8_t)((pn_len - 1) & 0x03);

    size_t pos = 0;
    out[pos++] = first_byte;

    /* DCID — 可变长度 */
    memcpy(out + pos, dst_cid->data, dst_cid->len); pos += dst_cid->len;

    /* PN */
    size_t pn_offset = pos;
    quic_pn_encode(out + pos, pn, pn_len); pos += pn_len;

    /* Payload */
    if (payload_len > 0) {
        memcpy(out + pos, payload, payload_len);
    }

    /* AAD includes up to and including PN (RFC 9001 §5.3) */
    size_t aad_len = pn_offset + pn_len;
    size_t ct_len;
    int ret = quic_crypto_encrypt(keys, pn,
                                  out /* aad */, aad_len,
                                  out + pos, payload_len,
                                  out + pos, &ct_len);
    if (ret < 0) {
        LOG_ERROR("[quic-packet] build_short: AEAD encrypt failed, pn=%llu aad_len=%zu"
                  " payload_len=%zu dcid_len=%u",
                  (unsigned long long)pn, aad_len, payload_len, dst_cid->len);
        return -1;
    }
    pos += ct_len;

    *out_len = pos;

    /* Header Protection */
    quic_crypto_hp_apply(keys, out, *out_len, pn_offset, pn_len);

    return 0;
}

int quic_packet_parse_short(const uint8_t *data, size_t len,
                            QuicConnectionId *dst_cid,
                            uint64_t *pn,
                            const uint8_t **payload, size_t *payload_len,
                            QuicCipherKeys *keys,
                            uint64_t expected_pn) {
    if (len < 1 + 1) {
        LOG_ERROR("[quic-packet] parse_short: len=%zu < 2", len);
        return -1;
    }
    if (data[0] & 0x80) {
        LOG_ERROR("[quic-packet] parse_short: not a short header, first_byte=0x%02x", data[0]);
        return -1;
    }

    /* 使用 dst_cid->len 作为预期 DCID 长度（调用方预填） */
    uint8_t dcid_len = dst_cid->len;
    if (dcid_len == 0) dcid_len = QUIC_CID_LEN;  /* 向后兼容 */
    if (len < 1 + dcid_len + 1) {
        LOG_ERROR("[quic-packet] parse_short: len=%zu too small for dcid_len=%u", len, dcid_len);
        return -1;
    }

    size_t pn_offset = 1 + dcid_len;
    if (pn_offset + 20 > len) {
        LOG_ERROR("[quic-packet] parse_short: not enough data for HP sample, pn_offset=%zu"
                  " len=%zu", pn_offset, len);
        return -1;
    }

    memcpy(dst_cid->data, data + 1, dcid_len);

    int pn_len_ret = quic_crypto_hp_remove_short(keys, (uint8_t*)data, len, pn_offset);
    if (pn_len_ret <= 0) {
        LOG_ERROR("[quic-packet] parse_short: HP remove failed, pn_offset=%zu len=%zu",
                  pn_offset, len);
        return -1;
    }
    size_t pn_len = (size_t)pn_len_ret;

    *pn = quic_pn_decode(data + pn_offset, pn_len, expected_pn);

    if (pn_offset + pn_len > len) {
        LOG_ERROR("[quic-packet] parse_short: PN past end, pn_offset=%zu pn_len=%zu len=%zu",
                  pn_offset, pn_len, len);
        return -1;
    }
    size_t ct_pos = pn_offset + pn_len;
    size_t ct_len_val = len - ct_pos;

    /* AAD includes up to and including PN (RFC 9001 §5.3) */
    int ret = quic_crypto_decrypt(keys, *pn,
                                  data /* aad */, pn_offset + pn_len,
                                  data + ct_pos, ct_len_val,
                                  (uint8_t*)(data + ct_pos), payload_len);
    if (ret < 0) {
        LOG_ERROR("[quic-packet] parse_short: AEAD decrypt failed, pn=%llu pn_len=%zu"
                  " ct_pos=%zu ct_len=%zu aad_len=%zu dcid_len=%u expected_pn=%llu",
                  (unsigned long long)*pn, pn_len, ct_pos, ct_len_val,
                  pn_offset + pn_len, dcid_len, (unsigned long long)expected_pn);
        return -1;
    }

    *payload = data + ct_pos;
    return 0;
}

/* ============================================
 * 帧 — CRYPTO (RFC 9000 §19.6)
 * ============================================ */

int quic_frame_write_crypto(uint8_t *buf, size_t cap,
                            uint64_t offset,
                            const uint8_t *data, size_t len) {
    size_t needed = 1 + quic_varint_len(offset) + quic_varint_len(len) + len;
    if (cap < needed) {
        LOG_ERROR("[quic-packet] write_crypto: cap=%zu < needed=%zu off=%llu len=%zu",
                  cap, needed, (unsigned long long)offset, len);
        return -1;
    }

    size_t pos = 0;
    buf[pos++] = QUIC_FRAME_CRYPTO;
    pos += quic_varint_encode(buf + pos, cap - pos, offset);
    pos += quic_varint_encode(buf + pos, cap - pos, len);
    memcpy(buf + pos, data, len); pos += len;
    return (int)pos;
}

int quic_frame_parse_crypto(const uint8_t *data, size_t len,
                            uint64_t *offset,
                            const uint8_t **crypto_data, size_t *crypto_len) {
    if (len < 1 || data[0] != QUIC_FRAME_CRYPTO) return -1;

    size_t pos = 1;
    uint64_t off;
    int vo = quic_varint_decode(data + pos, len - pos, &off);
    if (vo < 0) {
        LOG_ERROR("[quic-packet] parse_crypto: offset varint decode failed, pos=%zu len=%zu",
                  pos, len);
        return -1;
    }
    pos += vo;

    uint64_t clen;
    int vl = quic_varint_decode(data + pos, len - pos, &clen);
    if (vl < 0) {
        LOG_ERROR("[quic-packet] parse_crypto: length varint decode failed, pos=%zu len=%zu",
                  pos, len);
        return -1;
    }
    pos += vl;

    if (pos + clen > len) {
        LOG_ERROR("[quic-packet] parse_crypto: data overflow, pos=%zu clen=%llu len=%zu",
                  pos, (unsigned long long)clen, len);
        return -1;
    }

    *offset = off;
    *crypto_data = data + pos;
    *crypto_len = (size_t)clen;
    return (int)(pos + clen);
}

/* ============================================
 * 帧 — ACK (RFC 9000 §19.3)
 * ============================================ */

int quic_frame_write_ack(uint8_t *buf, size_t cap, const QuicAckFrame *ack) {
    /* 预估大小 */
    size_t needed = 1 + quic_varint_len(ack->largest_acknowledged)
                      + quic_varint_len(ack->ack_delay)
                      + quic_varint_len((uint64_t)ack->num_ranges) /* ack_range_count */
                      + quic_varint_len(ack->first_ack_range);
    for (int i = 0; i < ack->num_ranges; i++) {
        needed += quic_varint_len(ack->gap[i]) + quic_varint_len(ack->ack_range[i]);
    }
    if (cap < needed) {
        LOG_ERROR("[quic-packet] write_ack: cap=%zu < needed=%zu largest=%llu ranges=%d",
                  cap, needed, (unsigned long long)ack->largest_acknowledged, ack->num_ranges);
        return -1;
    }

    size_t pos = 0;
    buf[pos++] = QUIC_FRAME_ACK;
    pos += quic_varint_encode(buf + pos, cap - pos, ack->largest_acknowledged);
    pos += quic_varint_encode(buf + pos, cap - pos, ack->ack_delay);
    pos += quic_varint_encode(buf + pos, cap - pos, (uint64_t)ack->num_ranges);
    pos += quic_varint_encode(buf + pos, cap - pos, ack->first_ack_range);
    for (int i = 0; i < ack->num_ranges; i++) {
        pos += quic_varint_encode(buf + pos, cap - pos, ack->gap[i]);
        pos += quic_varint_encode(buf + pos, cap - pos, ack->ack_range[i]);
    }
    return (int)pos;
}

int quic_frame_parse_ack(const uint8_t *data, size_t len, QuicAckFrame *ack) {
    if (len < 1 || (data[0] != QUIC_FRAME_ACK && data[0] != QUIC_FRAME_ACK_ECN))
        return -1;

    size_t pos = 1;
    int v;
    uint64_t val;

    memset(ack, 0, sizeof(*ack));

    if (len - pos < 1) {
        LOG_ERROR("[quic-packet] parse_ack: truncated before largest_acknowledged, pos=%zu len=%zu",
                  pos, len);
        return -1;
    }
    v = quic_varint_decode(data + pos, len - pos, &ack->largest_acknowledged);
    if (v < 0) {
        LOG_ERROR("[quic-packet] parse_ack: largest_acknowledged varint decode failed, pos=%zu",
                  pos);
        return -1;
    }
    pos += v;

    if (len - pos < 1) {
        LOG_ERROR("[quic-packet] parse_ack: truncated before ack_delay, pos=%zu", pos);
        return -1;
    }
    v = quic_varint_decode(data + pos, len - pos, &ack->ack_delay);
    if (v < 0) {
        LOG_ERROR("[quic-packet] parse_ack: ack_delay varint decode failed, pos=%zu", pos);
        return -1;
    }
    pos += v;

    if (len - pos < 1) {
        LOG_ERROR("[quic-packet] parse_ack: truncated before ack_range_count, pos=%zu", pos);
        return -1;
    }
    v = quic_varint_decode(data + pos, len - pos, &val); /* ack_range_count */
    if (v < 0) {
        LOG_ERROR("[quic-packet] parse_ack: ack_range_count varint decode failed, pos=%zu", pos);
        return -1;
    }
    pos += v;
    uint64_t range_count = val;
    if (range_count > len - pos) {
        LOG_ERROR("[quic-packet] parse_ack: ack_range_count=%llu exceeds remaining=%zu",
                  (unsigned long long)range_count, len - pos);
        return -1;
    }

    if (len - pos < 1) {
        LOG_ERROR("[quic-packet] parse_ack: truncated before first_ack_range, pos=%zu", pos);
        return -1;
    }
    v = quic_varint_decode(data + pos, len - pos, &ack->first_ack_range);
    if (v < 0) {
        LOG_ERROR("[quic-packet] parse_ack: first_ack_range varint decode failed, pos=%zu", pos);
        return -1;
    }
    pos += v;

    for (uint64_t i = 0; i < range_count; i++) {
        uint64_t gap = 0, ar = 0;
        if (len - pos < 1) {
            LOG_ERROR("[quic-packet] parse_ack: truncated in gap[%llu], pos=%zu",
                      (unsigned long long)i, pos);
            return -1;
        }
        v = quic_varint_decode(data + pos, len - pos, &gap);
        if (v < 0) {
            LOG_ERROR("[quic-packet] parse_ack: gap[%llu] varint decode failed, pos=%zu",
                      (unsigned long long)i, pos);
            return -1;
        }
        pos += v;

        if (len - pos < 1) {
            LOG_ERROR("[quic-packet] parse_ack: truncated in ack_range[%llu], pos=%zu",
                      (unsigned long long)i, pos);
            return -1;
        }
        v = quic_varint_decode(data + pos, len - pos, &ar);
        if (v < 0) {
            LOG_ERROR("[quic-packet] parse_ack: ack_range[%llu] varint decode failed, pos=%zu",
                      (unsigned long long)i, pos);
            return -1;
        }
        pos += v;

        if (i < QUIC_ACK_MAX_RANGES) {
            ack->gap[i] = gap;
            ack->ack_range[i] = ar;
            ack->num_ranges = (int)(i + 1);
        }
    }

    /* ACK_ECN (0x03) 末尾还有 ECT0 / ECT1 / ECN-CE 三个计数 */
    if (data[0] == QUIC_FRAME_ACK_ECN) {
        for (int i = 0; i < 3; i++) {
            uint64_t cnt = 0;
            if (len - pos < 1) {
                LOG_ERROR("[quic-packet] parse_ack: truncated ECN count[%d], pos=%zu", i, pos);
                return -1;
            }
            v = quic_varint_decode(data + pos, len - pos, &cnt);
            if (v < 0) {
                LOG_ERROR("[quic-packet] parse_ack: ECN count[%d] varint failed, pos=%zu", i, pos);
                return -1;
            }
            pos += v;
        }
    }
    return (int)pos;
}

/* ============================================
 * 帧 — CONNECTION_CLOSE (RFC 9000 §19.19)
 * ============================================ */

int quic_frame_write_connection_close(uint8_t *buf, size_t cap,
                                      uint64_t error_code,
                                      const char *reason, size_t reason_len) {
    size_t needed = 1 + quic_varint_len(error_code)
                      + quic_varint_len(0) /* frame_type=0 for transport error */
                      + quic_varint_len(reason_len) + reason_len;
    if (cap < needed) {
        LOG_ERROR("[quic-packet] write_conn_close: cap=%zu < needed=%zu err=%llu reason_len=%zu",
                  cap, needed, (unsigned long long)error_code, reason_len);
        return -1;
    }

    size_t pos = 0;
    buf[pos++] = QUIC_FRAME_CONNECTION_CLOSE;
    pos += quic_varint_encode(buf + pos, cap - pos, error_code);
    pos += quic_varint_encode(buf + pos, cap - pos, 0); /* frame_type=0 */
    pos += quic_varint_encode(buf + pos, cap - pos, reason_len);
    if (reason_len > 0) {
        memcpy(buf + pos, reason, reason_len);
        pos += reason_len;
    }
    return (int)pos;
}

int quic_frame_parse_connection_close(const uint8_t *data, size_t len,
                                      uint64_t *error_code,
                                      const char **reason, size_t *reason_len) {
    if (len < 1) {
        LOG_ERROR("[quic-packet] parse_conn_close: len=%zu < 1", len);
        return -1;
    }
    int is_app = (data[0] == QUIC_FRAME_CONNECTION_CLOSE_APP);
    if (!is_app && data[0] != QUIC_FRAME_CONNECTION_CLOSE)
        return -1;

    size_t pos = 1;
    int v;
    uint64_t val;

    if (len - pos < 1) {
        LOG_ERROR("[quic-packet] parse_conn_close: truncated before error_code, pos=%zu", pos);
        return -1;
    }
    v = quic_varint_decode(data + pos, len - pos, error_code);
    if (v < 0) {
        LOG_ERROR("[quic-packet] parse_conn_close: error_code varint decode failed, pos=%zu", pos);
        return -1;
    }
    pos += v;

    /* frame_type 字段只在 CONNECTION_CLOSE (0x1c) 中存在;
     * CONNECTION_CLOSE_APP (0x1d) 直接跳到 reason_len */
    if (!is_app) {
        if (len - pos < 1) {
            LOG_ERROR("[quic-packet] parse_conn_close: truncated before frame_type, pos=%zu", pos);
            return -1;
        }
        v = quic_varint_decode(data + pos, len - pos, &val); /* frame_type */
        if (v < 0) {
            LOG_ERROR("[quic-packet] parse_conn_close: frame_type varint decode failed, pos=%zu", pos);
            return -1;
        }
        pos += v;
    }

    if (len - pos < 1) {
        LOG_ERROR("[quic-packet] parse_conn_close: truncated before reason_len, pos=%zu", pos);
        return -1;
    }
    v = quic_varint_decode(data + pos, len - pos, &val); /* reason_len */
    if (v < 0) {
        LOG_ERROR("[quic-packet] parse_conn_close: reason_len varint decode failed, pos=%zu", pos);
        return -1;
    }
    pos += v;
    *reason_len = (size_t)val;

    if (pos + *reason_len > len) {
        LOG_ERROR("[quic-packet] parse_conn_close: reason overflow, pos=%zu reason_len=%zu len=%zu",
                  pos, *reason_len, len);
        return -1;
    }

    *reason = (const char*)(data + pos);
    return (int)(pos + *reason_len);
}

/* ============================================
 * 帧 — PADDING (§19.1) / PING (§19.2)
 * ============================================ */

int quic_frame_write_padding(uint8_t *buf, size_t cap, size_t count) {
    if (cap < count) {
        LOG_ERROR("[quic-packet] write_padding: cap=%zu < count=%zu", cap, count);
        return -1;
    }
    memset(buf, 0, count);
    return (int)count;
}

int quic_frame_write_ping(uint8_t *buf, size_t cap) {
    if (cap < 1) {
        LOG_ERROR("[quic-packet] write_ping: cap=%zu < 1", cap);
        return -1;
    }
    buf[0] = QUIC_FRAME_PING;
    return 1;
}

/* ============================================
 * 帧 — STREAM (RFC 9000 §19.8)
 * ============================================ */

int quic_frame_write_stream(uint8_t *buf, size_t cap,
                            uint64_t stream_id, uint64_t offset, int fin,
                            const uint8_t *data, size_t data_len) {
    uint8_t type = QUIC_FRAME_STREAM | 0x04 /* OFF */ | 0x02 /* LEN */;
    if (fin) type |= 0x01;

    size_t needed = 1 + quic_varint_len(stream_id)
                      + quic_varint_len(offset)
                      + quic_varint_len(data_len)
                      + data_len;
    if (cap < needed) {
        LOG_ERROR("[quic-packet] write_stream: cap=%zu < needed=%zu stream=%llu off=%llu"
                  " len=%zu fin=%d",
                  cap, needed, (unsigned long long)stream_id, (unsigned long long)offset,
                  data_len, fin);
        return -1;
    }

    size_t pos = 0;
    buf[pos++] = type;
    pos += quic_varint_encode(buf + pos, cap - pos, stream_id);
    pos += quic_varint_encode(buf + pos, cap - pos, offset);
    pos += quic_varint_encode(buf + pos, cap - pos, data_len);
    memcpy(buf + pos, data, data_len);
    pos += data_len;
    return (int)pos;
}

int quic_frame_parse_stream(const uint8_t *data, size_t len,
                            uint64_t *stream_id, uint64_t *offset, int *fin,
                            const uint8_t **stream_data, size_t *stream_len) {
    if (len < 1) {
        LOG_ERROR("[quic-packet] parse_stream: len=%zu < 1", len);
        return -1;
    }
    uint8_t type = data[0];
    if ((type & 0xf8) != QUIC_FRAME_STREAM) return -1;

    int has_off = (type & 0x04) != 0;
    int has_len = (type & 0x02) != 0;
    *fin = (type & 0x01) != 0;

    size_t pos = 1;
    uint64_t val;
    int v;

    v = quic_varint_decode(data + pos, len - pos, stream_id);
    if (v < 0) {
        LOG_ERROR("[quic-packet] parse_stream: stream_id varint decode failed, pos=%zu", pos);
        return -1;
    }
    pos += v;

    if (has_off) {
        v = quic_varint_decode(data + pos, len - pos, offset);
        if (v < 0) {
            LOG_ERROR("[quic-packet] parse_stream: offset varint decode failed, pos=%zu", pos);
            return -1;
        }
        pos += v;
    } else {
        *offset = 0;
    }

    if (has_len) {
        v = quic_varint_decode(data + pos, len - pos, &val);
        if (v < 0) {
            LOG_ERROR("[quic-packet] parse_stream: length varint decode failed, pos=%zu", pos);
            return -1;
        }
        pos += v;
        *stream_len = (size_t)val;
    } else {
        *stream_len = len - pos;
    }

    if (pos + *stream_len > len) {
        LOG_ERROR("[quic-packet] parse_stream: data overflow, pos=%zu stream_len=%zu len=%zu"
                  " stream=%llu",
                  pos, *stream_len, len, (unsigned long long)*stream_id);
        return -1;
    }

    *stream_data = data + pos;
    return (int)(pos + *stream_len);
}

/* ============================================
 * 帧 — MAX_DATA (RFC 9000 §19.9)
 * ============================================ */

int quic_frame_write_max_data(uint8_t *buf, size_t cap, uint64_t max_data) {
    size_t needed = 1 + quic_varint_len(max_data);
    if (cap < needed) {
        LOG_ERROR("[quic-packet] write_max_data: cap=%zu < needed=%zu max_data=%llu",
                  cap, needed, (unsigned long long)max_data);
        return -1;
    }
    buf[0] = QUIC_FRAME_MAX_DATA;
    quic_varint_encode(buf + 1, cap - 1, max_data);
    return (int)needed;
}

int quic_frame_parse_max_data(const uint8_t *data, size_t len, uint64_t *max_data) {
    if (len < 1 || data[0] != QUIC_FRAME_MAX_DATA) return -1;
    int v = quic_varint_decode(data + 1, len - 1, max_data);
    if (v < 0) {
        LOG_ERROR("[quic-packet] parse_max_data: varint decode failed, len=%zu", len);
        return -1;
    }
    return 1 + v;
}

/* ============================================
 * 帧 — MAX_STREAM_DATA (RFC 9000 §19.10)
 * ============================================ */

int quic_frame_write_max_stream_data(uint8_t *buf, size_t cap,
                                     uint64_t stream_id, uint64_t max_data) {
    size_t needed = 1 + quic_varint_len(stream_id) + quic_varint_len(max_data);
    if (cap < needed) {
        LOG_ERROR("[quic-packet] write_max_stream_data: cap=%zu < needed=%zu stream=%llu"
                  " max=%llu",
                  cap, needed, (unsigned long long)stream_id, (unsigned long long)max_data);
        return -1;
    }
    size_t pos = 0;
    buf[pos++] = QUIC_FRAME_MAX_STREAM_DATA;
    pos += quic_varint_encode(buf + pos, cap - pos, stream_id);
    quic_varint_encode(buf + pos, cap - pos, max_data);
    return (int)needed;
}

int quic_frame_parse_max_stream_data(const uint8_t *data, size_t len,
                                     uint64_t *stream_id, uint64_t *max_data) {
    if (len < 1 || data[0] != QUIC_FRAME_MAX_STREAM_DATA) return -1;
    size_t pos = 1;
    int v = quic_varint_decode(data + pos, len - pos, stream_id);
    if (v < 0) {
        LOG_ERROR("[quic-packet] parse_max_stream_data: stream_id varint decode failed, pos=%zu",
                  pos);
        return -1;
    }
    pos += v;
    v = quic_varint_decode(data + pos, len - pos, max_data);
    if (v < 0) {
        LOG_ERROR("[quic-packet] parse_max_stream_data: max_data varint decode failed, pos=%zu",
                  pos);
        return -1;
    }
    return (int)(pos + v);
}

/* ============================================
 * 帧 — MAX_STREAMS (RFC 9000 §19.11)
 * ============================================ */

int quic_frame_write_max_streams(uint8_t *buf, size_t cap,
                                  uint64_t max_streams, int bidi) {
    size_t needed = 1 + quic_varint_len(max_streams);
    if (cap < needed) {
        LOG_ERROR("[quic-packet] write_max_streams: cap=%zu < needed=%zu max=%llu bidi=%d",
                  cap, needed, (unsigned long long)max_streams, bidi);
        return -1;
    }
    buf[0] = bidi ? QUIC_FRAME_MAX_STREAMS_BIDI : QUIC_FRAME_MAX_STREAMS_UNI;
    quic_varint_encode(buf + 1, cap - 1, max_streams);
    return (int)needed;
}

int quic_frame_parse_max_streams(const uint8_t *data, size_t len,
                                  uint64_t *max_streams, int *bidi) {
    if (len < 1) {
        LOG_ERROR("[quic-packet] parse_max_streams: len=%zu < 1", len);
        return -1;
    }
    if (data[0] == QUIC_FRAME_MAX_STREAMS_BIDI) {
        *bidi = 1;
    } else if (data[0] == QUIC_FRAME_MAX_STREAMS_UNI) {
        *bidi = 0;
    } else {
        return -1;
    }
    int v = quic_varint_decode(data + 1, len - 1, max_streams);
    if (v < 0) {
        LOG_ERROR("[quic-packet] parse_max_streams: varint decode failed, len=%zu type=0x%02x",
                  len, data[0]);
        return -1;
    }
    return 1 + v;
}

/* ============================================
 * 帧 — RESET_STREAM (RFC 9000 §19.4)
 * ============================================ */

int quic_frame_write_reset_stream(uint8_t *buf, size_t cap,
                                  uint64_t stream_id, uint64_t error_code,
                                  uint64_t final_size) {
    size_t needed = 1 + quic_varint_len(stream_id)
                      + quic_varint_len(error_code)
                      + quic_varint_len(final_size);
    if (cap < needed) {
        LOG_ERROR("[quic-packet] write_reset_stream: cap=%zu < needed=%zu stream=%llu"
                  " err=%llu final=%llu",
                  cap, needed, (unsigned long long)stream_id,
                  (unsigned long long)error_code, (unsigned long long)final_size);
        return -1;
    }
    size_t pos = 0;
    buf[pos++] = QUIC_FRAME_RESET_STREAM;
    pos += quic_varint_encode(buf + pos, cap - pos, stream_id);
    pos += quic_varint_encode(buf + pos, cap - pos, error_code);
    quic_varint_encode(buf + pos, cap - pos, final_size);
    return (int)needed;
}

int quic_frame_parse_reset_stream(const uint8_t *data, size_t len,
                                  uint64_t *stream_id, uint64_t *error_code,
                                  uint64_t *final_size) {
    if (len < 1 || data[0] != QUIC_FRAME_RESET_STREAM) return -1;
    size_t pos = 1;
    int v = quic_varint_decode(data + pos, len - pos, stream_id);
    if (v < 0) {
        LOG_ERROR("[quic-packet] parse_reset_stream: stream_id varint decode failed, pos=%zu",
                  pos);
        return -1;
    }
    pos += v;
    v = quic_varint_decode(data + pos, len - pos, error_code);
    if (v < 0) {
        LOG_ERROR("[quic-packet] parse_reset_stream: error_code varint decode failed, pos=%zu",
                  pos);
        return -1;
    }
    pos += v;
    v = quic_varint_decode(data + pos, len - pos, final_size);
    if (v < 0) {
        LOG_ERROR("[quic-packet] parse_reset_stream: final_size varint decode failed, pos=%zu",
                  pos);
        return -1;
    }
    return (int)(pos + v);
}

/* ============================================
 * 帧 — STOP_SENDING (RFC 9000 §19.5)
 * ============================================ */

int quic_frame_write_stop_sending(uint8_t *buf, size_t cap,
                                  uint64_t stream_id, uint64_t error_code) {
    size_t needed = 1 + quic_varint_len(stream_id) + quic_varint_len(error_code);
    if (cap < needed) {
        LOG_ERROR("[quic-packet] write_stop_sending: cap=%zu < needed=%zu stream=%llu err=%llu",
                  cap, needed, (unsigned long long)stream_id, (unsigned long long)error_code);
        return -1;
    }
    size_t pos = 0;
    buf[pos++] = QUIC_FRAME_STOP_SENDING;
    pos += quic_varint_encode(buf + pos, cap - pos, stream_id);
    quic_varint_encode(buf + pos, cap - pos, error_code);
    return (int)needed;
}

int quic_frame_parse_stop_sending(const uint8_t *data, size_t len,
                                  uint64_t *stream_id, uint64_t *error_code) {
    if (len < 1 || data[0] != QUIC_FRAME_STOP_SENDING) return -1;
    size_t pos = 1;
    int v = quic_varint_decode(data + pos, len - pos, stream_id);
    if (v < 0) {
        LOG_ERROR("[quic-packet] parse_stop_sending: stream_id varint decode failed, pos=%zu",
                  pos);
        return -1;
    }
    pos += v;
    v = quic_varint_decode(data + pos, len - pos, error_code);
    if (v < 0) {
        LOG_ERROR("[quic-packet] parse_stop_sending: error_code varint decode failed, pos=%zu",
                  pos);
        return -1;
    }
    return (int)(pos + v);
}

/* ============================================
 * 帧 — NEW_CONNECTION_ID (RFC 9000 §19.15)
 * ============================================ */

int quic_frame_parse_new_conn_id(const uint8_t *data, size_t len,
                                  uint64_t *seq, uint64_t *retire_prior_to,
                                  uint8_t *cid_len, const uint8_t **cid_data,
                                  const uint8_t **reset_token) {
    if (len < 1 || data[0] != QUIC_FRAME_NEW_CONNECTION_ID) return -1;

    size_t pos = 1;
    int v;

    /* Sequence Number */
    v = quic_varint_decode(data + pos, len - pos, seq);
    if (v < 0) return -1;
    pos += v;

    /* Retire Prior To */
    v = quic_varint_decode(data + pos, len - pos, retire_prior_to);
    if (v < 0) return -1;
    pos += v;

    /* Connection ID Length (8-bit) + Connection ID + Stateless Reset Token (16) */
    if (pos >= len) return -1;
    *cid_len = data[pos++];
    *cid_data = data + pos;
    if (pos + *cid_len + 16 > len) return -1;
    *reset_token = data + pos + *cid_len;
    pos += *cid_len + 16;

    return (int)pos;
}

/* ============================================
 * 帧 — NEW_TOKEN (RFC 9000 §19.7)
 * ============================================ */

int quic_frame_parse_new_token(const uint8_t *data, size_t len) {
    if (len < 1 || data[0] != QUIC_FRAME_NEW_TOKEN) return -1;

    size_t pos = 1;
    uint64_t token_len;

    int v = quic_varint_decode(data + pos, len - pos, &token_len);
    if (v < 0) return -1;
    pos += v;

    if (pos + token_len > len) return -1;
    pos += (size_t)token_len;

    return (int)pos;
}

/* ============================================
 * RETIRE_CONNECTION_ID (RFC 9000 §19.16)
 * ============================================ */

int quic_frame_write_retire_conn_id(uint8_t *buf, size_t cap,
                                     uint64_t seq_num) {
    size_t needed = 1 + quic_varint_len(seq_num);
    if (cap < needed) {
        LOG_ERROR("[quic-packet] write_retire_conn_id: cap=%zu < needed=%zu seq=%llu",
                  cap, needed, (unsigned long long)seq_num);
        return -1;
    }
    buf[0] = QUIC_FRAME_RETIRE_CONNECTION_ID;
    quic_varint_encode(buf + 1, cap - 1, seq_num);
    return (int)needed;
}

int quic_frame_parse_retire_conn_id(const uint8_t *data, size_t len,
                                     uint64_t *seq_num) {
    if (len < 1 || data[0] != QUIC_FRAME_RETIRE_CONNECTION_ID) return -1;
    int v = quic_varint_decode(data + 1, len - 1, seq_num);
    if (v < 0) {
        LOG_ERROR("[quic-packet] parse_retire_conn_id: varint decode failed");
        return -1;
    }
    return 1 + v;
}

/* ============================================
 * BLOCKED frames (RFC 9000 §19.14–19.17)
 *
 * 所有 BLOCKED 帧都有相同的 wire format:
 *   type | limit_value
 * STREAM_DATA_BLOCKED 额外携带 stream_id
 * ============================================ */

/* ── DATA_BLOCKED (§19.14) ────────────────── */

int quic_frame_write_data_blocked(uint8_t *buf, size_t cap,
                                   uint64_t max_data) {
    size_t needed = 1 + quic_varint_len(max_data);
    if (cap < needed) {
        LOG_ERROR("[quic-packet] write_data_blocked: cap=%zu < needed=%zu max=%llu",
                  cap, needed, (unsigned long long)max_data);
        return -1;
    }
    buf[0] = QUIC_FRAME_DATA_BLOCKED;
    quic_varint_encode(buf + 1, cap - 1, max_data);
    return (int)needed;
}

int quic_frame_parse_data_blocked(const uint8_t *data, size_t len,
                                   uint64_t *max_data) {
    if (len < 1 || data[0] != QUIC_FRAME_DATA_BLOCKED) return -1;
    int v = quic_varint_decode(data + 1, len - 1, max_data);
    if (v < 0) {
        LOG_ERROR("[quic-packet] parse_data_blocked: varint decode failed");
        return -1;
    }
    return 1 + v;
}

/* ── STREAM_DATA_BLOCKED (§19.15) ─────────── */

int quic_frame_write_stream_data_blocked(uint8_t *buf, size_t cap,
                                          uint64_t stream_id,
                                          uint64_t max_stream_data) {
    size_t needed = 1 + quic_varint_len(stream_id)
                      + quic_varint_len(max_stream_data);
    if (cap < needed) {
        LOG_ERROR("[quic-packet] write_stream_data_blocked: cap=%zu < needed=%zu"
                  " stream=%llu limit=%llu",
                  cap, needed, (unsigned long long)stream_id,
                  (unsigned long long)max_stream_data);
        return -1;
    }
    size_t pos = 0;
    buf[pos++] = QUIC_FRAME_STREAM_DATA_BLOCKED;
    pos += quic_varint_encode(buf + pos, cap - pos, stream_id);
    quic_varint_encode(buf + pos, cap - pos, max_stream_data);
    return (int)needed;
}

int quic_frame_parse_stream_data_blocked(const uint8_t *data, size_t len,
                                          uint64_t *stream_id,
                                          uint64_t *max_stream_data) {
    if (len < 1 || data[0] != QUIC_FRAME_STREAM_DATA_BLOCKED) return -1;
    size_t pos = 1;
    int v = quic_varint_decode(data + pos, len - pos, stream_id);
    if (v < 0) {
        LOG_ERROR("[quic-packet] parse_stream_data_blocked: stream_id decode failed");
        return -1;
    }
    pos += v;
    v = quic_varint_decode(data + pos, len - pos, max_stream_data);
    if (v < 0) {
        LOG_ERROR("[quic-packet] parse_stream_data_blocked: limit decode failed");
        return -1;
    }
    return (int)(pos + v);
}

/* ── STREAMS_BLOCKED_BIDI / UNI (§19.16/17) ── */

int quic_frame_write_streams_blocked(uint8_t *buf, size_t cap,
                                      uint64_t max_streams, int bidi) {
    size_t needed = 1 + quic_varint_len(max_streams);
    if (cap < needed) {
        LOG_ERROR("[quic-packet] write_streams_blocked: cap=%zu < needed=%zu"
                  " max=%llu bidi=%d",
                  cap, needed, (unsigned long long)max_streams, bidi);
        return -1;
    }
    buf[0] = bidi ? QUIC_FRAME_STREAMS_BLOCKED_BIDI
                  : QUIC_FRAME_STREAMS_BLOCKED_UNI;
    quic_varint_encode(buf + 1, cap - 1, max_streams);
    return (int)needed;
}

int quic_frame_parse_streams_blocked(const uint8_t *data, size_t len,
                                      uint64_t *max_streams, int *bidi) {
    if (len < 1) return -1;
    if (data[0] == QUIC_FRAME_STREAMS_BLOCKED_BIDI) {
        *bidi = 1;
    } else if (data[0] == QUIC_FRAME_STREAMS_BLOCKED_UNI) {
        *bidi = 0;
    } else {
        return -1;
    }
    int v = quic_varint_decode(data + 1, len - 1, max_streams);
    if (v < 0) {
        LOG_ERROR("[quic-packet] parse_streams_blocked: varint decode failed");
        return -1;
    }
    return 1 + v;
}

/* ============================================
 * Retry 包 (RFC 9000 §17.2.5 + RFC 9001 §5.8)
 * ============================================ */

int quic_packet_build_retry(uint8_t *out, size_t *out_len,
                             const QuicConnectionId *original_dcid,
                             const QuicConnectionId *scid,
                             const uint8_t *token, size_t token_len) {
    size_t pos = 0;
    /* Header: 1(1) | 0x03(type) | 0 */
    out[pos++] = 0xF0;  /* 1111 0000 → long header + type=Retry */
    /* Version */
    uint32_t ver_be = htonl(QUIC_VERSION_V1);
    memcpy(out + pos, &ver_be, 4); pos += 4;
    /* DCIL + DCID */
    out[pos++] = original_dcid->len;
    memcpy(out + pos, original_dcid->data, original_dcid->len);
    pos += original_dcid->len;
    /* SCIL + SCID (服务端生成的新 CID) */
    out[pos++] = scid->len;
    memcpy(out + pos, scid->data, scid->len);
    pos += scid->len;
    /* Retry Token (无长度前缀 — 长度由包总长隐式确定) */
    memcpy(out + pos, token, token_len); pos += token_len;

    /* 计算 pseudo-packet 用于完整性标签 */
    uint8_t pseudo[512];
    size_t pp = 0;
    pseudo[pp++] = original_dcid->len;
    memcpy(pseudo + pp, original_dcid->data, original_dcid->len);
    pp += original_dcid->len;
    pseudo[pp++] = 0xF0;
    memcpy(pseudo + pp, &ver_be, 4); pp += 4;
    pseudo[pp++] = original_dcid->len;
    memcpy(pseudo + pp, original_dcid->data, original_dcid->len);
    pp += original_dcid->len;
    pseudo[pp++] = scid->len;
    memcpy(pseudo + pp, scid->data, scid->len);
    pp += scid->len;
    memcpy(pseudo + pp, token, token_len); pp += token_len;

    uint8_t tag[16];
    if (quic_crypto_compute_retry_tag(pseudo, pp, tag) < 0) {
        LOG_ERROR("[quic-packet] retry tag computation failed");
        return -1;
    }
    memcpy(out + pos, tag, 16); pos += 16;

    *out_len = pos;
    LOG_DEBUG("[quic-packet] built Retry: odcid=%u scid=%u token=%zu total=%zu",
             original_dcid->len, scid->len, token_len, pos);
    return 0;
}

/* ============================================
 * DATAGRAM 帧 (RFC 9221 §4)
 * ============================================ */

int quic_frame_write_datagram(uint8_t *buf, size_t cap,
                               const uint8_t *data, size_t len) {
    /* type=0x31 (带长度) — 1 + varint(len) + len */
    size_t lv_sz = quic_varint_len((uint64_t)len);
    size_t needed = 1 + lv_sz + len;
    if (cap < needed) return -1;

    size_t pos = 0;
    buf[pos++] = QUIC_FRAME_DATAGRAM_LEN;
    pos += quic_varint_encode(buf + pos, cap - pos, (uint64_t)len);
    memcpy(buf + pos, data, len); pos += len;
    return (int)pos;
}

int quic_frame_parse_datagram(const uint8_t *data, size_t len,
                               const uint8_t **payload, size_t *payload_len) {
    if (len < 1) return -1;
    uint8_t ftype = data[0];

    if (ftype == QUIC_FRAME_DATAGRAM) {
        /* 不带长度 — 占据剩余全部字节 */
        *payload = data + 1;
        *payload_len = len - 1;
        return (int)len;
    }

    if (ftype == QUIC_FRAME_DATAGRAM_LEN) {
        /* 带长度 */
        uint64_t dlen;
        int v = quic_varint_decode(data + 1, len - 1, &dlen);
        if (v < 0) return -1;
        size_t pos = (size_t)(1 + v);
        if (pos + (size_t)dlen > len) return -1;
        *payload = data + pos;
        *payload_len = (size_t)dlen;
        return (int)(pos + (size_t)dlen);
    }

    return -1;
}
