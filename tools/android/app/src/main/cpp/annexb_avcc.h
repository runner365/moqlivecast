#ifndef ANNEXB_AVCC_H
#define ANNEXB_AVCC_H

/* ============================================================
 * Annex-B → AVCC —— Android 推流路上唯一一处**真的要自己写**的编解码处理。
 *
 * 为什么需要它：MediaCodec 的 H.264 输出是 **Annex-B**（NALU 之间用
 * `00 00 01` / `00 00 00 01` 起始码分隔），而 moq-lite 这条线要的是 **AVCC**
 * （每个 NALU 前面放它自己的**长度**，4 字节大端）。两者是同一批 NALU 的两种
 * 打包方式，字节布局完全不同 —— 直接把 Annex-B 交给 SDK，对端会把它当成
 * 「第一个 NAL 长度 = 0x00000001」然后整帧解不开。
 *
 * ⚠️ 长度前缀的宽度由 avcC 的 `lengthSizeMinusOne` 声明，这里是 4（＝默认、
 * 也是 MediaCodec 的惯例）。构 avcC 的 build_avcc_from_csd() 写死 `ff`
 * （0xff = 保留 6 位全 1 + lengthSizeMinusOne=3 → 4 字节），两边必须一致。
 * ============================================================ */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 把一段 Annex-B 转成 AVCC，写进 dst。
 *
 * 返回写出的字节数；<0 = 失败（dst 装不下 / 输入里一个起始码都没有）。
 *
 * ⚠️ dst 的容量请给 `src_len + src_len / 3 + 64`：
 *   3 字节起始码换成 4 字节长度前缀，每个 NALU 最多**多 1 个字节**，
 *   而 NALU 至少 1 字节、起始码 3 字节 ⇒ NALU 个数 ≤ src_len / 4。
 *   上面那个式子留了足够余量。给不够这里会返回 <0，不会越界写。 */
int annexb_to_avcc(const uint8_t *src, size_t src_len,
                   uint8_t *dst, size_t dst_cap);

/* 由 MediaCodec 的 codec-specific data 拼出 MP4 的 **avcC** 记录 ——
 * 也就是 catalog 里那条 rendition 的 `description`（本库会把它转成十六进制）。
 *
 * csd0 通常是 SPS（有时 SPS+PPS 连在一起），csd1 通常是 PPS。两个都按
 * Annex-B 解，取**第一个** SPS(nal_type=7) 和**第一个** PPS(nal_type=8)。
 *
 * 返回写出的字节数；<0 = 失败（找不到 SPS 或 PPS / dst 装不下）。
 * dst 给 128 字节足够（典型 avcC 十几到三十几字节）。 */
int build_avcc_from_csd(const uint8_t *csd0, size_t csd0_len,
                        const uint8_t *csd1, size_t csd1_len,
                        uint8_t *dst, size_t dst_cap);

#ifdef __cplusplus
}
#endif

#endif /* ANNEXB_AVCC_H */
