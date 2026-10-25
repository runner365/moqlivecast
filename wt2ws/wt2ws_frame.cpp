#include "wt2ws_frame.hpp"

#include <cstring>

namespace cpp_streamer {
namespace wt2ws {

bool FrameCodec::Feed(const uint8_t *data, size_t len) {
    if (data && len > 0) {
        buf_.insert(buf_.end(), data, data + len);
    }
    /* A single frame can be at most max_payload_ + header.  More than that
     * means either a bogus length or a peer that never completes a frame. */
    if (buffered() > max_payload_ + kFrameHeaderLen) {
        return false;
    }
    return true;
}

FrameCodec::ParseResult FrameCodec::Next(uint8_t *type,
                                         std::vector<uint8_t> *payload) {
    if (buffered() < kFrameHeaderLen) {
        return ParseResult::kNeedMore;
    }
    const uint8_t *p = buf_.data() + off_;
    const uint32_t len = (static_cast<uint32_t>(p[0]) << 24) |
                         (static_cast<uint32_t>(p[1]) << 16) |
                         (static_cast<uint32_t>(p[2]) << 8) |
                         static_cast<uint32_t>(p[3]);
    if (len > max_payload_) {
        return ParseResult::kError;
    }
    if (buffered() < kFrameHeaderLen + len) {
        return ParseResult::kNeedMore;
    }
    *type = p[4];
    payload->assign(p + kFrameHeaderLen, p + kFrameHeaderLen + len);
    off_ += kFrameHeaderLen + len;
    Compact();
    return ParseResult::kOk;
}

void FrameCodec::Compact() {
    if (off_ == 0) return;
    if (off_ == buf_.size()) {
        buf_.clear();
        off_ = 0;
    } else if (off_ >= 64 * 1024 || off_ * 2 >= buf_.size()) {
        buf_.erase(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(off_));
        off_ = 0;
    }
}

void FrameCodec::Encode(uint8_t type, const uint8_t *data, size_t len,
                        std::vector<uint8_t> *out) {
    out->resize(kFrameHeaderLen + len);
    uint8_t *p = out->data();
    p[0] = static_cast<uint8_t>((len >> 24) & 0xff);
    p[1] = static_cast<uint8_t>((len >> 16) & 0xff);
    p[2] = static_cast<uint8_t>((len >> 8) & 0xff);
    p[3] = static_cast<uint8_t>(len & 0xff);
    p[4] = type;
    if (len > 0 && data) {
        std::memcpy(p + kFrameHeaderLen, data, len);
    }
}

} /* namespace wt2ws */
} /* namespace cpp_streamer */
