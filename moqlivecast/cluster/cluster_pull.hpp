#ifndef CLUSTER_PULL_HPP
#define CLUSTER_PULL_HPP

#include "cluster_common.hpp"
#include "moq_catalog.hpp"

#include "moq_lite_client.h"

#include <map>
#include <set>
#include <string>
#include <vector>

namespace cpp_streamer {

/* Edge 的按需回源链路：**一条本地缺流一条 QUIC 连接**（role=SUBSCRIBER）。
 *
 * 流程：连接 → 按已知广播名订阅 catalog.json → ClaimPublisher + SetCatalogJson
 * → 订阅各媒体轨 → 把帧重建成 FLV tag body 写入 MediaStreamManager。
 * 本地播放器（moq-lite / RTMP / HTTP-FLV）从 MediaStreamManager 取流，与本地
 * 推流完全同路。
 *
 * 本地推流优先：发现同 key 的本地 publisher 就放弃回源；每次写帧前校验 owner。
 */
class ClusterPullLink {
public:
    ClusterPullLink(uv_loop_t *loop, const ClusterOptions &opts,
                    std::string local_key, std::string wire,
                    std::string owner_id);
    ~ClusterPullLink();

    ClusterPullLink(const ClusterPullLink &) = delete;
    ClusterPullLink &operator=(const ClusterPullLink &) = delete;

    /* 有需求：启动或复活链路。 */
    void Start();
    void TouchDemand(int64_t now_ms);
    /* 最终停止：释放本地 publisher、关连接。 */
    void Stop();
    void OnTick(int64_t now_ms);

    bool Stopped() const { return stopped_; }
    bool FullyStopped() const { return stopped_ && session_ == 0; }
    /* 还没拿到 catalog 就超时了（manager 据此写负缓存）。 */
    bool AcquireTimedOut() const { return acquire_timed_out_; }
    const std::string &Key() const { return local_key_; }
    int64_t LastDemandMs() const { return last_demand_ms_; }

private:
    static void OnSessionConnected(moq_lite_session_t s, int status, void *user);
    static void OnSessionClosed(moq_lite_session_t s, int err,
                                const char *reason, void *user);
    static void OnSubscribed(moq_lite_sub_t sub, uint64_t first_group, void *user);
    static void OnFrame(moq_lite_sub_t sub, uint64_t group, uint64_t frame_index,
                        int64_t ts_delta, const uint8_t *payload, size_t len,
                        void *user);
    static void OnSubscribeEnd(moq_lite_sub_t sub, uint64_t end_group, void *user);
    static void OnDrop(moq_lite_sub_t sub, uint64_t gstart, uint64_t gend,
                       uint64_t error_code, void *user);
    static void OnGroupEnd(moq_lite_sub_t sub, uint64_t group, void *user);

    void CreateSession();
    void HandleConnected();
    void HandleClosed(int err);
    void HandleCatalogFrame(const uint8_t *payload, size_t len);
    void HandleMediaFrame(moq_lite_sub_t sub, uint64_t group,
                          uint64_t frame_index, const uint8_t *payload,
                          size_t len);

    /* 组乱序重排。组流是独立的 QUIC 流，会乱序到达；直接按到达顺序写进
     * MediaStreamManager 会让同一轨的 dts 回退（实测 ±5s），下游 RTMP
     * muxer 直接断开。 */
    struct PullFrame {
        uint64_t ts_us = 0;
        bool key = false;
        std::vector<uint8_t> data;
    };
    struct PullTrackState {
        uint64_t next_group = 0;
        bool started = false;
        std::map<uint64_t, std::vector<PullFrame>> pending;
        std::set<uint64_t> ended;
        int64_t oldest_pending_ms = 0;
    };
    void HandleTrackFrame(const std::string &name, uint64_t group,
                          uint64_t ts_us, bool key, const uint8_t *data,
                          size_t len);
    void OnGroupEndInternal(const std::string &name, uint64_t group);
    void DrainPending(const std::string &name);
    void EmitFrame(const std::string &name, const PullFrame &frame);
    void SendSequenceHeader(const std::string &name);
    void SubscribeTrack(const std::string &name);
    void FreeSessionIfClosed(int64_t now_ms);
    void ClearSessionHandles();
    void AbandonLocalWins();

private:
    uv_loop_t *loop_ = nullptr;
    ClusterOptions opts_;
    std::string local_key_;
    std::string wire_;
    std::string owner_id_;
    std::string app_;
    std::string stream_;

    moq_lite_session_t session_ = 0;
    moq_lite_broadcast_t bc_ = 0;
    std::map<uint32_t, std::string> sub_track_;   /* sub 句柄 → track 名 */
    std::map<std::string, uint32_t> track_sub_;   /* track 名 → sub 句柄 */
    std::map<std::string, std::string> seq_hdr_sig_; /* 已发序列头的 config 签名 */
    std::map<std::string, int64_t> last_dts_ms_;    /* 每轨上一帧 dts，查回退/重复 */
    int64_t dts_warn_ms_ = 0;
    std::map<std::string, PullTrackState> track_state_;

    MoqCatalogInfo catalog_;
    std::string catalog_json_;
    bool claimed_ = false;

    bool linked_ = false;
    bool connecting_ = false;
    bool stopped_ = false;
    bool close_pending_ = false;
    bool session_dead_ = false;
    bool reconnect_requested_ = false;
    bool acquire_timed_out_ = false;

    int64_t started_at_ms_ = 0;
    /* 整个「一次回源」的总预算，**跨重连不重置**：否则订阅被中心 reset
     * （流不存在）后会每次重连都重新计时，永不放弃、日志刷屏。 */
    bool acquire_deadline_set_ = false;
    int64_t acquire_deadline_ms_ = 0;
    int64_t last_demand_ms_ = 0;
    int64_t last_media_ms_ = 0;
    int64_t free_at_ms_ = 0;
    int64_t retry_at_ms_ = 0;
    int64_t base_ms_ = -1;
    int attempt_ = 0;
};

} /* namespace cpp_streamer */

#endif /* CLUSTER_PULL_HPP */
