#ifndef MEDIA_OVER_WT_MOQ_PUSH_HPP
#define MEDIA_OVER_WT_MOQ_PUSH_HPP

#include "wt_server/wt_server.hpp"
#include "media_packet.hpp"
#include "data_buffer.hpp"

#include <cstdint>
#include <memory>
#include <queue>
#include <string>
#include <unordered_map>
#include <vector>

namespace cpp_streamer {

class MoqPushSession : public AvWriterInterface {
public:
    void OnData(WTServerSession &sess, WTServerStream &st,
                const uint8_t *data, size_t len);
    void Close();

    const std::string &App() const { return app_; }
    const std::string &Stream() const { return stream_; }
    bool IsPublisher() const { return publisher_; }
    bool IsSubscriber() const { return subscriber_; }
    bool HasMedia() const { return !app_.empty() && !stream_.empty(); }

    /* 会话归属：PUBLISH 到达时用 session id 去 MediaStreamManager 认领流。 */
    WTServerSession *Session() const { return sess_; }
    void SetSession(WTServerSession &sess) { sess_ = &sess; session_id_ = sess.SessionId(); }
    const std::string &SessionId() const { return session_id_; }
    /* 本会话是否仍是该流的 publisher。被后来者顶掉后返回 false，
     * 媒体路径据此丢弃在途对象（见 draft-ietf-moq-transport §2.4.3）。 */
    bool IsPublisherOwner() const;

    void AttachPlayer(const std::string &writer_id);
    void DetachPlayer();

    virtual int WritePacket(Media_Packet_Ptr pkt) override;
    virtual std::string GetKey() override;
    virtual std::string GetWriterId() override;
    virtual void CloseWriter() override;
    virtual bool IsInited() override;
    virtual void SetInitFlag(bool flag) override;

private:
    struct StreamState {
        std::vector<uint8_t> buf;
        /* CONTROL : 控制单向流，只有 SETUP / GOAWAY
         * REQUEST : 请求双向流，首条消息为 PUBLISH 或 SUBSCRIBE；
         *           响应（SUBSCRIBE_OK 等）走本流反向
         * DATA    : 对象流（SUBGROUP + OBJECT）
         * 订阅方向的下行流不再经过这里：它由本端发起（OpenUniDownlink），
         * 只写不收，不会收到 on_stream_data。
         * 见 draft-ietf-moq-transport §3.3。 */
        enum Kind { UNKNOWN, CONTROL, REQUEST, DATA } kind = UNKNOWN;
        bool header_done = false;
        bool has_props = false;
        uint64_t alias = 0;
        uint64_t last_obj = 0;
        bool last_obj_set = false;
    };

    struct Downlink {
        WTServerStream *st = nullptr;
        bool header_sent = false;
        uint64_t obj = 0;
        bool wait_key = false;
        bool congested = false;
        std::queue<std::shared_ptr<DataBuffer>> send_q;
        size_t send_q_bytes = 0;
        int64_t last_congested_dbg_ts_s = 0;
        int64_t last_send_dbg_ts_s = 0;
    };

    StreamState &StateOf(WTServerStream &st);
    void Pump(WTServerSession &sess, WTServerStream &st, StreamState &ss);
    bool ParseControl(WTServerSession &sess, WTServerStream &st, StreamState &ss);
    bool ParseSubgroupHeader(StreamState &ss);
    bool ParseObject(StreamState &ss);
    bool ParsePublish(const uint8_t *body, size_t len);
    bool ParseSubscribe(const uint8_t *body, size_t len,
                        uint64_t &request_id, uint64_t &alias, std::string &name);
    /* 应答走【请求流自身的反向】（draft §3.3.2：发送响应的一端必须把
     * 对应的响应消息发回该请求流），因此 st 传请求流本身。 */
    void SendSubscribeOk(WTServerStream &st, uint64_t request_id, uint64_t alias);
    /* 订阅方向的数据单向流由【服务端】发起（对象走单向流）—— 客户端不再
     * 开双向 bind 流。返回 false 表示该 track 的流暂未开出来（通常是客户端
     * MAX_STREAMS_UNI 配额还没到），由媒体路径重试，不是致命错误。 */
    bool OpenUniDownlink(WTServerSession &sess, uint64_t alias);
    void SendGop(Downlink &dl, uint64_t alias);
    void EnsurePullTsBase();
    int64_t PullTs(Media_Packet_Ptr pkt);
    void EmitLoc(Downlink &dl, uint64_t alias, Media_Packet_Ptr pkt);
    void DrainSendQueue(Downlink &dl);
    void EnqueueSend(Downlink &dl, std::shared_ptr<DataBuffer> buf);
    void TrimSendQueue(Downlink &dl);
    void ClearSendQueue(Downlink &dl);
    /* cts_ms = pts - dts，来自 LOC 私有扩展属性 0x0e；无该属性时为 0 */
    void OnLocObject(uint64_t alias, int64_t ts_ms, int64_t cts_ms, bool key,
                     const uint8_t *cfg, size_t cfg_len,
                     const uint8_t *payload, size_t payload_len);
    void EmitFlvVideo(int64_t dts, bool seq, bool key,
                      const uint8_t *data, size_t len, int64_t cts_ms);
    void EmitFlvAudio(int64_t dts, bool seq, const uint8_t *data, size_t len);

    std::string app_;
    std::string stream_;
    std::string writer_id_;
    /* 本会话（PUBLISH 来源）。被顶替时要断开的就是它。 */
    WTServerSession *sess_ = nullptr;
    std::string session_id_;
    bool publisher_ = false;
    bool subscriber_ = false;
    bool player_added_ = false;
    bool init_flag_ = false;
    bool writer_closed_ = false;
    bool avc_seq_sent_ = false;
    bool aac_seq_sent_ = false;
    /* 对端的控制单向流（只读，承载 SETUP）。 */
    WTServerStream *control_ = nullptr;
    /* 订阅方所属 session —— 媒体路径重试开单向流时要用（见 OpenUniDownlink）。 */
    WTServerSession *subscriber_sess_ = nullptr;
    Downlink video_dl_;
    Downlink audio_dl_;
    int64_t pull_ts_base_ = -1;
    int64_t last_pull_ts_ = -1;
    /* 被顶掉之后的丢包日志限流（秒），见 EmitFlvVideo / EmitFlvAudio。 */
    int64_t last_owner_drop_dbg_ts_s_ = 0;
    std::unordered_map<WTServerStream *, StreamState> streams_;
};

class MoqRfcHandler {
public:
    static void OnSession(WTServerSession &sess, const std::string &path);
    static void OnStreamData(WTServerSession &sess, WTServerStream &st,
                             const uint8_t *data, size_t len,
                             const std::string &path);
    static void OnSessionClose(WTServerSession &sess, const std::string &path);
    static bool IsMoqPath(const std::string &path);
    static void AddSubscriber(WTServerSession &sess, MoqPushSession &ms);
    static void RemoveSubscriber(WTServerSession &sess);
    /* 同名流「新顶旧」：断开被顶掉的旧 publisher 会话。只做日志 + 断连，
     * 不带错误码 —— 带码通知要等 PUBLISH_DONE / REQUEST_ERROR（见
     * docs/moq-transport-gap.md G6），当前版本先保证一条流只有一个源。 */
    static bool KickPublisherSession(const std::string &session_id,
                                     const std::string &stream_key);

private:
    static std::unordered_map<std::string, std::unique_ptr<MoqPushSession>> sessions_;
};

} /* namespace cpp_streamer */

#endif /* MEDIA_OVER_WT_MOQ_PUSH_HPP */
