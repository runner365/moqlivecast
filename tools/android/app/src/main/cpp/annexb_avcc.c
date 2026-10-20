#include "annexb_avcc.h"

#include <string.h>

/* 起始码的长度：3（00 00 01）或 4（00 00 00 01），没有则 0。
 * ⚠️ 先判 3 字节：`00 00 00 01` 在 3 字节判据下第三字节是 0 不是 1，不匹配，
 * 于是会落到 4 字节分支 —— 顺序反了也得到同样结果，但先 3 后 4 是惯例。
 * 防竞争字节（00 00 03 xx）不会误判：那一段第三字节是 3，不是 1。 */
static size_t startcode_len(const uint8_t *p, size_t n) {
    if (n >= 3 && p[0] == 0 && p[1] == 0 && p[2] == 1) return 3;
    if (n >= 4 && p[0] == 0 && p[1] == 0 && p[2] == 0 && p[3] == 1) return 4;
    return 0;
}

int annexb_to_avcc(const uint8_t *src, size_t src_len,
                   uint8_t *dst, size_t dst_cap) {
    if (!src || !dst || src_len == 0) return -1;

    /* 1) 找第一个起始码，跳过它前面的任何前导字节。
     *    正常不会有多余前导，但 MediaCodec 的 csd 有时带 1 个填充字节。 */
    size_t i = 0;
    int found = 0;
    while (i < src_len) {
        size_t sc = startcode_len(src + i, src_len - i);
        if (sc) { i += sc; found = 1; break; }
        i++;
    }
    if (!found) return -1;   /* 一个起始码都没有 → 不是 Annex-B */

    /* 2) 逐段搬：从当前 NALU 起点扫到下一个起始码。 */
    size_t o = 0;
    for (;;) {
        size_t j = i;
        while (j < src_len && startcode_len(src + j, src_len - j) == 0) j++;

        const size_t nal_len = j - i;
        if (nal_len > 0) {
            /* 长度前缀是 4 字节大端，与 build_avcc_from_csd 里的 lengthSizeMinusOne
             * = 3（即 4 字节）必须一致。 */
            if (o + 4 + nal_len > dst_cap) return -1;
            dst[o++] = (uint8_t)((nal_len >> 24) & 0xff);
            dst[o++] = (uint8_t)((nal_len >> 16) & 0xff);
            dst[o++] = (uint8_t)((nal_len >> 8) & 0xff);
            dst[o++] = (uint8_t)(nal_len & 0xff);
            memcpy(dst + o, src + i, nal_len);
            o += nal_len;
        }

        if (j >= src_len) break;
        i = j + startcode_len(src + j, src_len - j);
        if (i >= src_len) break;    /* 末尾一个空段，正常收尾 */
    }
    return (int)o;
}

/* 在 Annex-B 缓冲里找第一个 nal_type ∈ {want} 的 NALU（不含起始码）。
 * 返回 1 = 找到（out/out_len 指向 NALU 本身），0 = 没有。 */
static int find_nalu(const uint8_t *p, size_t n, int want,
                     const uint8_t **out, size_t *out_len) {
    if (!p || n == 0) return 0;

    size_t i = 0;
    int found = 0;
    while (i < n) {
        size_t sc = startcode_len(p + i, n - i);
        if (sc) { i += sc; found = 1; break; }
        i++;
    }
    if (!found) return 0;

    for (;;) {
        size_t j = i;
        while (j < n && startcode_len(p + j, n - j) == 0) j++;

        if (j > i) {
            /* NAL header 的低 5 位是 nal_unit_type */
            if ((p[i] & 0x1f) == (uint8_t)want) {
                *out = p + i;
                *out_len = j - i;
                return 1;
            }
        }
        if (j >= n) return 0;
        i = j + startcode_len(p + j, n - j);
        if (i >= n) return 0;
    }
}

int build_avcc_from_csd(const uint8_t *csd0, size_t csd0_len,
                        const uint8_t *csd1, size_t csd1_len,
                        uint8_t *dst, size_t dst_cap) {
    if (!dst) return -1;

    const uint8_t *sps = NULL, *pps = NULL;
    size_t sps_len = 0, pps_len = 0;

    /* SPS/PPS 可能都在 csd0（拼在一起），也可能分在 csd0/csd1 —— 两种都试。 */
    if (!find_nalu(csd0, csd0_len, 7, &sps, &sps_len))
        find_nalu(csd1, csd1_len, 7, &sps, &sps_len);
    if (!find_nalu(csd0, csd0_len, 8, &pps, &pps_len))
        find_nalu(csd1, csd1_len, 8, &pps, &pps_len);

    /* 没有 SPS/PPS 就构不出 avcC。返回 <0 而不是糊一个空壳：catalog 里放一个
     * 假 description，对端建解码器时才会炸，离这里很远。 */
    if (!sps || !pps) return -1;
    if (sps_len < 4 || sps_len > 0xffff || pps_len == 0 || pps_len > 0xffff)
        return -1;
    if (dst_cap < 11 + sps_len + pps_len) return -1;

    size_t o = 0;
    dst[o++] = 0x01;                    /* configurationVersion */
    dst[o++] = sps[1];                  /* AVCProfileIndication   = profile_idc  */
    dst[o++] = sps[2];                  /* profile_compatibility  = 约束标志     */
    dst[o++] = sps[3];                  /* AVCLevelIndication     = level_idc    */
    dst[o++] = 0xff;                    /* 6 位保留(全1) | lengthSizeMinusOne=3 → 4 字节 */
    dst[o++] = 0xe1;                    /* 3 位保留(全1) | numOfSequenceParameterSets=1 */
    dst[o++] = (uint8_t)((sps_len >> 8) & 0xff);
    dst[o++] = (uint8_t)(sps_len & 0xff);
    memcpy(dst + o, sps, sps_len);
    o += sps_len;
    dst[o++] = 0x01;                    /* numOfPictureParameterSets = 1 */
    dst[o++] = (uint8_t)((pps_len >> 8) & 0xff);
    dst[o++] = (uint8_t)(pps_len & 0xff);
    memcpy(dst + o, pps, pps_len);
    o += pps_len;

    return (int)o;
}
