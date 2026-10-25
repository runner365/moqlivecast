#ifndef WT2WS_SESSION_HPP
#define WT2WS_SESSION_HPP

#include "wt2ws_backend.hpp"
#include "wt2ws_config.hpp"
#include "wt2ws_frame.hpp"

#include "webtransport_server_api.h"

#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <vector>

namespace cpp_streamer {
namespace wt2ws {

/* One WebTransport session <-> one WebSocket backend connection.
 *
 * Inbound: WT byte stream -> FrameCodec -> WS text/binary frame.
 * Outbound: WS text/binary frame -> FrameCodec -> WT byte stream.
 *
 * The payload is opaque; only frame boundaries and the text/binary bit are
 * interpreted.  Both directions are reliable, so backpressure (rather than
 * dropping) is the policy: bytes that the WT stream refuses (write returns 1)
 * stay in outq_ until on_writable fires.
 */
class Wt2wsSession : public std::enable_shared_from_this<Wt2wsSession> {
public:
    Wt2wsSession(uv_loop_t *loop, const Wt2wsConfig &cfg, wt_session_t *sess);
    ~Wt2wsSession();

    Wt2wsSession(const Wt2wsSession &) = delete;
    Wt2wsSession &operator=(const Wt2wsSession &) = delete;

    void Start();
    void AttachStream(wt_stream_t *st);
    void OnWtData(const uint8_t *data, size_t len);
    void OnWtWritable();
    void OnWtClosed();
    void OnTick(int64_t now_ms);

    bool closed() const { return closed_; }
    uint64_t id() const { return id_; }
    wt_stream_t *stream() const { return stream_; }

private:
    void OnBackendConnected();
    void OnBackendText(const std::string &text);
    void OnBackendBinary(const uint8_t *data, size_t len);
    void OnBackendClosed(int code, const std::string &desc);

    bool QueueInbound(uint8_t type, const uint8_t *data, size_t len);
    void FlushInbound();
    bool EnqueueOut(uint8_t type, const uint8_t *data, size_t len);
    void FlushOut();
    void Fail(const std::string &reason);
    void Touch();
    void ScheduleBackendCleanup();

    static void OnWritableTramp(wt_stream_t *st, void *user);
    static void OnCleanupTramp(uv_timer_t *timer);

    uv_loop_t *loop_ = nullptr;
    Wt2wsConfig cfg_;
    wt_session_t *sess_ = nullptr;
    wt_stream_t *stream_ = nullptr;
    uint64_t id_ = 0;

    std::unique_ptr<Wt2wsBackend> backend_;
    FrameCodec codec_;

    struct InMsg {
        uint8_t type = 0;
        std::string data;
    };
    std::deque<InMsg> inq_;
    size_t inq_bytes_ = 0;

    struct OutMsg {
        std::vector<uint8_t> data;
        size_t off = 0;
    };
    std::deque<OutMsg> outq_;
    size_t out_bytes_ = 0;

    int64_t last_activity_ms_ = 0;
    /* The reused WebSocketClient does not surface TCP connect failures
     * (TcpClient::OnUVClientConnected returns silently), so guard the
     * handshake with a deadline instead of hanging forever. */
    int64_t connect_deadline_ms_ = 0;
    uv_timer_t cleanup_timer_;
    bool cleanup_scheduled_ = false;
    bool closed_ = false;
    bool wt_closed_ = false;
};

} /* namespace wt2ws */
} /* namespace cpp_streamer */

#endif /* WT2WS_SESSION_HPP */
