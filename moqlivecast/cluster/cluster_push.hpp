#ifndef CLUSTER_PUSH_HPP
#define CLUSTER_PUSH_HPP

#include "cluster_common.hpp"
#include "media_packet.hpp"
#include "moq_catalog.hpp"

#include "moq_lite_client.h"

#include <map>
#include <string>

namespace cpp_streamer {

/* Edge → Center 的上推链路：**一条本地流一条 QUIC 连接**（role=PUBLISHER）。
 *
 * 生命周期：
 *   本地流出现 catalog → 建立连接 → publish 一条广播 + N 条轨 → 注册 writer，
 *   把 Media_Packet 剥成裸 AVCC/AAC 后喂给 C 客户端。
 *   本地 unpublish → Pause（只摘 writer，连接保留一段时间，覆盖推流端重启）；
 *   超过宽限期仍无 catalog → Stop（关连接，中心据此释放）。
 *   连接断开 → 按退避重连；catalog / 轨句柄保留，重连后重新 publish。
 */
class ClusterPushLink {
public:
    ClusterPushLink(uv_loop_t *loop, const ClusterOptions &opts,
                    std::string local_key, std::string wire,
                    std::string catalog_json);
    ~ClusterPushLink();

    ClusterPushLink(const ClusterPushLink &) = delete;
    ClusterPushLink &operator=(const ClusterPushLink &) = delete;

    void Start();
    /* 本地流暂时没 publisher：摘掉 writer，连接保留。 */
    void Pause();
    /* 本地流（重新）出现 catalog：更新轨并重新接上 writer。 */
    void Resume(const std::string &catalog_json);
    /* 彻底停止：摘 writer、关连接。之后等 tick 释放 session 再删对象。 */
    void Stop();
    void OnTick(int64_t now_ms);

    bool Stopped() const { return stopped_; }
    /* stopped 且 session 已释放 —— manager 可以安全 erase 了。 */
    bool FullyStopped() const { return stopped_ && session_ == 0; }
    /* 本地 publisher 已消失、连接还在宽限期里。此时这条 PushLink 不能当成
     * 「本地有源」来挡回源。 */
    bool Paused() const { return paused_; }
    const std::string &Key() const { return local_key_; }
    int64_t PausedMs() const { return paused_at_ms_; }

private:
    class Writer;

    static void OnSessionConnected(moq_lite_session_t s, int status, void *user);
    static void OnSessionClosed(moq_lite_session_t s, int err,
                                const char *reason, void *user);

    void CreateSession();
    void HandleConnected();
    void HandleClosed(int err);
    void PublishRenditions();
    void AttachWriter();
    void DetachWriter();
    void FreeSessionIfClosed(int64_t now_ms);

    int WritePacket(Media_Packet_Ptr pkt);

private:
    uv_loop_t *loop_ = nullptr;
    ClusterOptions opts_;
    std::string local_key_;
    std::string wire_;
    std::string catalog_json_;
    MoqCatalogInfo catalog_;

    moq_lite_session_t session_ = 0;
    moq_lite_broadcast_t bc_ = 0;
    std::map<std::string, moq_lite_track_t> track_by_name_;
    moq_lite_track_t video_track_ = 0;
    moq_lite_track_t audio_track_ = 0;

    Writer *writer_ = nullptr;
    bool writer_added_ = false;
    bool linked_ = false;
    bool connecting_ = false;
    bool stopped_ = false;
    bool close_pending_ = false;
    bool session_dead_ = false;
    bool paused_ = false;
    int64_t paused_at_ms_ = 0;
    int64_t free_at_ms_ = 0;
    int64_t retry_at_ms_ = 0;
    int attempt_ = 0;
};

} /* namespace cpp_streamer */

#endif /* CLUSTER_PUSH_HPP */
