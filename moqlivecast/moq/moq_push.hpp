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
        enum Kind { UNKNOWN, CONTROL, DATA, DOWNLINK } kind = UNKNOWN;
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
        int64_t last_drained_dbg_ts_s = 0;
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
    void SendSubscribeOk(WTServerStream &st, uint64_t request_id, uint64_t alias);
    void BindDownlink(WTServerSession &sess, WTServerStream &st, uint64_t alias);
    void SendGop(Downlink &dl, uint64_t alias);
    void EnsurePullTsBase();
    int64_t PullTs(Media_Packet_Ptr pkt);
    void EmitLoc(Downlink &dl, uint64_t alias, Media_Packet_Ptr pkt);
    void DrainSendQueue(Downlink &dl);
    void EnqueueSend(Downlink &dl, std::shared_ptr<DataBuffer> buf);
    void TrimSendQueue(Downlink &dl);
    void ClearSendQueue(Downlink &dl);
    void OnLocObject(uint64_t alias, int64_t ts_ms, bool key,
                     const uint8_t *cfg, size_t cfg_len,
                     const uint8_t *payload, size_t payload_len);
    void EmitFlvVideo(int64_t dts, bool seq, bool key,
                      const uint8_t *data, size_t len);
    void EmitFlvAudio(int64_t dts, bool seq, const uint8_t *data, size_t len);

    std::string app_;
    std::string stream_;
    std::string writer_id_;
    bool publisher_ = false;
    bool subscriber_ = false;
    bool player_added_ = false;
    bool init_flag_ = false;
    bool writer_closed_ = false;
    bool avc_seq_sent_ = false;
    bool aac_seq_sent_ = false;
    WTServerStream *control_ = nullptr;
    Downlink video_dl_;
    Downlink audio_dl_;
    int64_t pull_ts_base_ = -1;
    int64_t last_pull_ts_ = -1;
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

private:
    static std::unordered_map<std::string, std::unique_ptr<MoqPushSession>> sessions_;
};

} /* namespace cpp_streamer */

#endif /* MEDIA_OVER_WT_MOQ_PUSH_HPP */
