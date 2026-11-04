#include "websocket_flv_writer.hpp"

#include "byte_stream.hpp"
#include "flv_pub.hpp"
#include "net/http/websocket/websocket_session.hpp"
#include "utils/timeex.hpp"

#include <cstring>
#include <vector>

namespace cpp_streamer {

WebSocketFlvWriter::WebSocketFlvWriter(std::string key, std::string id,
                                       std::shared_ptr<WebSocketSession> session,
                                       Logger* logger)
    : key_(std::move(key))
    , writer_id_(std::move(id))
    , session_(session)
    , logger_(logger) {
    alive_ms_ = GetNowMilliSec();
}

WebSocketFlvWriter::~WebSocketFlvWriter() {
    CloseWriter();
}

int WebSocketFlvWriter::AppendFlvHeader(std::vector<uint8_t>& out) {
    if (flv_header_ready_) return 0;
    /* FLV header: 'FLV', version 1, flags (audio|video), DataOffset=9,
     * PreviousTagSize0=0. */
    static const uint8_t hdr[13] = {
        0x46, 0x4c, 0x56, 0x01, 0x05,
        0x00, 0x00, 0x00, 0x09,
        0x00, 0x00, 0x00, 0x00
    };
    out.insert(out.end(), hdr, hdr + sizeof(hdr));
    flv_header_ready_ = true;
    return 0;
}

int WebSocketFlvWriter::WritePacket(Media_Packet_Ptr pkt) {
    if (!pkt || !pkt->buffer_ptr_) return -1;
    if (closed_flag_) return -1;

    auto session = session_.lock();
    if (!session || !session->IsConnected()) {
        closed_flag_ = true;
        return -1;
    }

    const size_t payload_size = pkt->buffer_ptr_->DataLen();
    if (payload_size > 0xffffff) {
        LogErrorf(logger_, "ws-flv payload too large: %zu", payload_size);
        return -1;
    }

    uint8_t tag[11];
    if (pkt->av_type_ == MEDIA_VIDEO_TYPE) {
        tag[0] = FLV_TAG_VIDEO;
    } else if (pkt->av_type_ == MEDIA_AUDIO_TYPE) {
        tag[0] = FLV_TAG_AUDIO;
    } else {
        LogWarnf(logger_, "ws-flv unsupported av type:%d", (int)pkt->av_type_);
        return -1;
    }

    const uint32_t ts = (uint32_t)(pkt->dts_ < 0 ? 0 : pkt->dts_);
    tag[1] = (uint8_t)((payload_size >> 16) & 0xff);
    tag[2] = (uint8_t)((payload_size >> 8) & 0xff);
    tag[3] = (uint8_t)(payload_size & 0xff);
    tag[4] = (uint8_t)((ts >> 16) & 0xff);
    tag[5] = (uint8_t)((ts >> 8) & 0xff);
    tag[6] = (uint8_t)(ts & 0xff);
    tag[7] = (uint8_t)((ts >> 24) & 0xff);
    tag[8] = 0;
    tag[9] = 0;
    tag[10] = 0;

    std::vector<uint8_t> out;
    out.reserve(13 + sizeof(tag) + payload_size + 4);
    AppendFlvHeader(out);
    out.insert(out.end(), tag, tag + sizeof(tag));
    out.insert(out.end(), pkt->buffer_ptr_->Data(),
               pkt->buffer_ptr_->Data() + payload_size);

    uint32_t pre_size = (uint32_t)(sizeof(tag) + payload_size);
    uint8_t pre[4] = {
        (uint8_t)((pre_size >> 24) & 0xff),
        (uint8_t)((pre_size >> 16) & 0xff),
        (uint8_t)((pre_size >> 8) & 0xff),
        (uint8_t)(pre_size & 0xff)
    };
    out.insert(out.end(), pre, pre + sizeof(pre));

    session->AsyncWriteData(out.data(), out.size());
    alive_ms_ = GetNowMilliSec();
    return 0;
}

std::string WebSocketFlvWriter::GetKey() { return key_; }
std::string WebSocketFlvWriter::GetWriterId() { return writer_id_; }

void WebSocketFlvWriter::CloseWriter() {
    auto session = session_.lock();
    if (session) {
        /* Clearing the callback before CloseSession is mandatory: this writer
         * is about to be deleted, and CloseSession may invoke cb_->OnClose(). */
        session->SetSessionCallback(nullptr);
        if (!closed_flag_) session->CloseSession();
    }
    closed_flag_ = true;
    session_.reset();
}

bool WebSocketFlvWriter::IsInited() { return init_flag_; }
void WebSocketFlvWriter::SetInitFlag(bool flag) { init_flag_ = flag; }

bool WebSocketFlvWriter::IsAlive() {
    if (closed_flag_) return false;
    auto session = session_.lock();
    if (!session || !session->IsConnected()) {
        closed_flag_ = true;
        return false;
    }
    /* Same policy as HTTP-FLV: don't tear down a player that is waiting for
     * the first publisher packet.  Once media has flowed, 30s without a tag
     * means the stream/session is gone. */
    if (!init_flag_) return true;
    if (alive_ms_ <= 0) {
        alive_ms_ = GetNowMilliSec();
        return true;
    }
    return (GetNowMilliSec() - alive_ms_) <= 30000;
}

void WebSocketFlvWriter::OnReadData(int code, const uint8_t* data, size_t len) {
    (void)code; (void)data; (void)len;
}
void WebSocketFlvWriter::OnReadText(int code, const std::string& text) {
    (void)code; (void)text;
}
void WebSocketFlvWriter::OnClose(int code, const std::string& desc) {
    (void)code; (void)desc;
    closed_flag_ = true;
}

} /* namespace cpp_streamer */
