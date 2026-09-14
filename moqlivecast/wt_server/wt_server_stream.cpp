#include "wt_server/wt_server_stream.hpp"
#include "wt_server/wt_server_session.hpp"

namespace cpp_streamer {

WTServerStream::WTServerStream(wt_stream_t *st, WTServerSession *sess)
    : st_(st)
    , sess_(sess) {
    if (st_) {
        wt_stream_set_user_data(st_, this);
    }
}

WTServerStream::~WTServerStream() {
    if (st_ && wt_stream_get_user_data(st_) == this) {
        wt_stream_set_on_writable(st_, nullptr, nullptr);
        wt_stream_set_user_data(st_, nullptr);
    }
    st_ = nullptr;
    sess_ = nullptr;
}

void WTServerStream::Feed(const uint8_t *data, size_t len) {
    if (!st_ || !data || len == 0) return;
    if (on_data_) {
        on_data_(*this, data, len);
    }
}

void WTServerStream::SetOnWritable(OnWritable cb) {
    on_writable_ = std::move(cb);
    if (!st_) return;
    if (on_writable_) {
        wt_stream_set_on_writable(st_, OnWritableTrampoline, this);
    } else {
        wt_stream_set_on_writable(st_, nullptr, nullptr);
    }
}

void WTServerStream::OnWritableTrampoline(wt_stream_t *st, void *user) {
    auto *self = static_cast<WTServerStream *>(user);
    if (!self || self->st_ != st) return;
    if (self->on_writable_) self->on_writable_(*self);
}

int WTServerStream::Write(const uint8_t *data, size_t len) {
    if (!st_ || !data || len == 0) return -1;
    return wt_stream_write(st_, data, len);
}

void WTServerStream::Detach() {
    if (st_ && wt_stream_get_user_data(st_) == this) {
        wt_stream_set_on_writable(st_, nullptr, nullptr);
        wt_stream_set_user_data(st_, nullptr);
    }
    st_ = nullptr;
    sess_ = nullptr;
}

WTServerStream *WTServerStream::From(wt_stream_t *st) {
    if (!st) return nullptr;
    return static_cast<WTServerStream *>(wt_stream_get_user_data(st));
}

} /* namespace cpp_streamer */
