#include "wt2ws_session.hpp"

#include "logger.h"

#include <utility>

namespace cpp_streamer {
namespace wt2ws {

Wt2wsSession::Wt2wsSession(uv_loop_t *loop, const Wt2wsConfig &cfg,
                           wt_session_t *sess)
    : loop_(loop)
    , cfg_(cfg)
    , sess_(sess)
    , id_(sess ? wt_session_get_id(sess) : 0)
    , codec_(cfg.max_frame) {
    last_activity_ms_ = static_cast<int64_t>(uv_now(loop_));
    connect_deadline_ms_ = last_activity_ms_ + cfg.connect_timeout_ms;
    uv_timer_init(loop_, &cleanup_timer_);
    cleanup_timer_.data = this;
}

Wt2wsSession::~Wt2wsSession() {
    if (backend_) backend_->Close();
    /* Retired sessions are normally kept until process exit, so this is only
     * reached on shutdown; close the handle to keep the loop clean. */
    uv_timer_stop(&cleanup_timer_);
    uv_close(reinterpret_cast<uv_handle_t *>(&cleanup_timer_), nullptr);
}

void Wt2wsSession::Start() {
    auto self = shared_from_this();
    Wt2wsBackend::Sink sink;
    sink.on_connected = [self]() { self->OnBackendConnected(); };
    sink.on_text = [self](const std::string &t) { self->OnBackendText(t); };
    sink.on_binary = [self](const uint8_t *d, size_t n) {
        self->OnBackendBinary(d, n);
    };
    sink.on_closed = [self](int c, const std::string &d) {
        self->OnBackendClosed(c, d);
    };
    backend_.reset(new Wt2wsBackend(loop_, cfg_, std::move(sink)));
    backend_->Connect();
}

void Wt2wsSession::AttachStream(wt_stream_t *st) {
    if (closed_ || wt_closed_ || !st || stream_) return;
    stream_ = st;
    wt_stream_set_on_writable(st, &Wt2wsSession::OnWritableTramp, this);
    LOG_INFO("[wt2ws] session %llu stream attached",
             static_cast<unsigned long long>(id_));
    FlushOut();
}

void Wt2wsSession::OnWtData(const uint8_t *data, size_t len) {
    if (closed_ || wt_closed_ || len == 0) return;
    Touch();
    if (!codec_.Feed(data, len)) {
        Fail("inbound frame exceeds --max-frame");
        return;
    }
    while (!closed_) {
        uint8_t type = 0;
        std::vector<uint8_t> payload;
        const FrameCodec::ParseResult r = codec_.Next(&type, &payload);
        if (r == FrameCodec::ParseResult::kNeedMore) break;
        if (r == FrameCodec::ParseResult::kError) {
            Fail("frame length exceeds --max-frame");
            return;
        }
        if (type != kFrameText && type != kFrameBinary) {
            Fail("unknown frame type");
            return;
        }
        if (backend_ && backend_->connected()) {
            if (type == kFrameText) {
                backend_->SendText(std::string(payload.begin(), payload.end()));
            } else {
                backend_->SendBinary(payload.data(), payload.size());
            }
        } else if (!QueueInbound(type, payload.data(), payload.size())) {
            Fail("inbound queue overflow");
            return;
        }
    }
}

void Wt2wsSession::OnWtWritable() {
    FlushOut();
}

void Wt2wsSession::OnWtClosed() {
    if (wt_closed_) return;
    wt_closed_ = true;
    closed_ = true;
    LOG_INFO("[wt2ws] session %llu WT closed (in=%zu out=%zu queued)",
             static_cast<unsigned long long>(id_), inq_bytes_, out_bytes_);
    if (backend_) backend_->Close();
    ScheduleBackendCleanup();
}

void Wt2wsSession::OnTick(int64_t now_ms) {
    if (closed_) return;
    if ((!backend_ || !backend_->connected()) && now_ms >= connect_deadline_ms_) {
        Fail("backend connect timeout");
        return;
    }
    if (now_ms - last_activity_ms_ > cfg_.idle_timeout_ms) {
        Fail("idle timeout");
    }
}

void Wt2wsSession::OnBackendConnected() {
    Touch();
    connect_deadline_ms_ = 0; /* no longer pending */
    FlushInbound();
}

void Wt2wsSession::OnBackendText(const std::string &text) {
    Touch();
    EnqueueOut(kFrameText, reinterpret_cast<const uint8_t *>(text.data()),
               text.size());
}

void Wt2wsSession::OnBackendBinary(const uint8_t *data, size_t len) {
    Touch();
    EnqueueOut(kFrameBinary, data, len);
}

void Wt2wsSession::OnBackendClosed(int code, const std::string &desc) {
    (void)code;
    (void)desc;
    if (closed_) return;
    Fail("backend closed");
}

bool Wt2wsSession::QueueInbound(uint8_t type, const uint8_t *data, size_t len) {
    if (inq_bytes_ + len > cfg_.max_queue) return false;
    InMsg m;
    m.type = type;
    m.data.assign(reinterpret_cast<const char *>(data), len);
    inq_bytes_ += len;
    inq_.push_back(std::move(m));
    return true;
}

void Wt2wsSession::FlushInbound() {
    if (!backend_ || !backend_->connected()) return;
    while (!inq_.empty() && !closed_) {
        InMsg &m = inq_.front();
        if (m.type == kFrameText) {
            backend_->SendText(m.data);
        } else {
            backend_->SendBinary(reinterpret_cast<const uint8_t *>(m.data.data()),
                                 m.data.size());
        }
        inq_bytes_ -= m.data.size();
        inq_.pop_front();
    }
}

bool Wt2wsSession::EnqueueOut(uint8_t type, const uint8_t *data, size_t len) {
    if (out_bytes_ + len + kFrameHeaderLen > cfg_.max_queue) {
        Fail("outbound queue overflow");
        return false;
    }
    OutMsg m;
    FrameCodec::Encode(type, data, len, &m.data);
    out_bytes_ += m.data.size();
    outq_.push_back(std::move(m));
    FlushOut();
    return !closed_;
}

void Wt2wsSession::FlushOut() {
    if (closed_ || wt_closed_ || !stream_) return;
    while (!outq_.empty() && !closed_) {
        OutMsg &m = outq_.front();
        const size_t remain = m.data.size() - m.off;
        const int r = wt_stream_write(stream_, m.data.data() + m.off, remain);
        if (r == 0) {
            out_bytes_ -= remain;
            outq_.pop_front();
        } else if (r == 1) {
            /* Congested: wait for on_writable. */
            break;
        } else {
            Fail("WT stream write failed");
            return;
        }
    }
}

void Wt2wsSession::Fail(const std::string &reason) {
    if (closed_) return;
    closed_ = true;
    LOG_WARN("[wt2ws] session %llu closing: %s",
             static_cast<unsigned long long>(id_), reason.c_str());
    if (backend_) backend_->Close();
    ScheduleBackendCleanup();
    if (sess_ && !wt_closed_) {
        wt_session_close(sess_);
    }
}

void Wt2wsSession::ScheduleBackendCleanup() {
    if (cleanup_scheduled_) return;
    cleanup_scheduled_ = true;
    uv_timer_start(&cleanup_timer_, &Wt2wsSession::OnCleanupTramp, 1, 0);
}

void Wt2wsSession::OnCleanupTramp(uv_timer_t *timer) {
    Wt2wsSession *self = static_cast<Wt2wsSession *>(timer->data);
    if (!self) return;
    self->cleanup_scheduled_ = false;
    self->backend_.reset(); /* TcpClient destructor closes the socket handle */
}

void Wt2wsSession::Touch() {
    last_activity_ms_ = static_cast<int64_t>(uv_now(loop_));
}

void Wt2wsSession::OnWritableTramp(wt_stream_t *st, void *user) {
    (void)st;
    Wt2wsSession *self = static_cast<Wt2wsSession *>(user);
    if (self) self->OnWtWritable();
}

} /* namespace wt2ws */
} /* namespace cpp_streamer */
