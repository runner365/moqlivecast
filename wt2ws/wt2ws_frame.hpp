#ifndef WT2WS_FRAME_HPP
#define WT2WS_FRAME_HPP

#include <cstddef>
#include <cstdint>
#include <vector>

namespace cpp_streamer {
namespace wt2ws {

/* Wire framing on the WebTransport leg.
 *
 * WebSocket is message oriented; a WebTransport stream is a byte stream.
 * The proxy therefore needs an explicit frame boundary:
 *
 *     +--------+--------+------------------+
 *     | u32 BE | u8     | payload          |
 *     | length | type   | (length bytes)   |
 *     +--------+--------+------------------+
 *
 * `length` counts the payload only (not the 5-byte header).  `type` is
 * 1 = text, 2 = binary, matching the two WebSocket data frame kinds the
 * relay forwards.  JSON/audio semantics stay above this layer -- the
 * proxy never parses the payload.
 */
constexpr uint8_t kFrameText = 1;
constexpr uint8_t kFrameBinary = 2;
constexpr size_t kFrameHeaderLen = 5;

class FrameCodec {
public:
    enum class ParseResult {
        kOk,       /* one frame was written to type/payload */
        kNeedMore, /* incomplete frame, feed more bytes */
        kError,    /* declared length exceeds the configured limit */
    };

    explicit FrameCodec(size_t max_payload) : max_payload_(max_payload) {}

    /* Append bytes received from the WebTransport stream.
     * Returns false only when the pending buffer exceeds max_payload + header;
     * the caller should close the session in that case. */
    bool Feed(const uint8_t *data, size_t len);

    ParseResult Next(uint8_t *type, std::vector<uint8_t> *payload);

    size_t buffered() const { return buf_.size() - off_; }
    size_t max_payload() const { return max_payload_; }

    static void Encode(uint8_t type, const uint8_t *data, size_t len,
                       std::vector<uint8_t> *out);

private:
    void Compact();

    std::vector<uint8_t> buf_;
    size_t off_ = 0;
    size_t max_payload_ = 0;
};

} /* namespace wt2ws */
} /* namespace cpp_streamer */

#endif /* WT2WS_FRAME_HPP */
