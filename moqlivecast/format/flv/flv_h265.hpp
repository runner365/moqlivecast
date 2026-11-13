#ifndef FLV_H265_HPP
#define FLV_H265_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace cpp_streamer {

/* Enhanced-RTMP H.265 helpers.
 *
 * Internal MediaStreamManager representation for H.265 is the enhanced-RTMP
 * ExVideo body, never the non-standard legacy CodecID=0x0C form:
 *
 *   [0x80 | frame<<4 | packet_type]["hvc1"] + payload
 *
 * SequenceStart has no CompositionTime; CodedFrames has a signed SI24
 * CompositionTime before the length-prefixed HEVC NALUs.  Legacy 0x0C input
 * is normalised to this form at ingest. */

bool IsHevcDecoderConfig(const uint8_t *cfg, size_t len);
std::string HevcCodecString(const uint8_t *hvcC, size_t len);

/* Parse/convert a video tag body.  `out` receives an enhanced H.265 body and
 * `payload_off` points at the codec payload (hvcC or HEVC access unit).
 * Returns false for non-H.265 or malformed input. */
bool NormalizeH265FlvVideoBody(const uint8_t *body, size_t len,
                               std::vector<uint8_t> *out,
                               size_t *payload_off);

/* Build an enhanced-RTMP H.265 body. */
std::vector<uint8_t> MakeH265ExVideoBody(bool seq, bool key, int64_t cts_ms,
                                         const uint8_t *payload,
                                         size_t payload_len);

} /* namespace cpp_streamer */

#endif /* FLV_H265_HPP */
