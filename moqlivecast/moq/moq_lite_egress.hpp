#ifndef MOQ_LITE_EGRESS_HPP
#define MOQ_LITE_EGRESS_HPP

#include "media_packet.hpp"
#include "quic_connection.h"

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace cpp_streamer {

/* 一条「对端订阅我们」的**媒体**轨的下行写入器。
 *
 * 一个实例 = 一条订阅（对端的某一个 subscribe id）+ 它对应的那条 track。
 * 它同时是 MediaStreamManager 的一个 player：注册之后每来一个媒体包都会走
 * WritePacket，由本类编成 moq-lite 的 GROUP（uni 流）+ FRAME。
 *
 * ⚠️ 为什么是**每条订阅**一个实例，而不是每条 track 一个：组号（GROUP 头里的
 * sequence）在这套实现里是**按订阅自己编**的 —— SUBSCRIBE_START 里报的
 * first_group 就是本实例的第一个组号，之后 0,1,2… 自增。参考实现的组号来自
 * track 级的生产者状态（两个订阅方看到同一批号），但那对我们**不可观测**：
 * 组号只作定序/去重键，播放时间轴完全由 FRAME 的差值（与 Legacy 内层微秒
 * 前缀）决定。按订阅编的好处是全套状态都是本实例私有的，没有跨会话共享的
 * 可变状态 —— 少一类只在「同一 track 两个订阅方」时才现形的 bug。
 *
 * 线程：只在 QUIC loop 线程里被调用（MediaStreamManager 的扇出与 QUIC 回调
 * 同线程）。**不得阻塞**：拿不到 uni 流额度时丢组，不进等待。 */
class MoqLiteEgress : public AvWriterInterface {
public:
    /* conn 不持有所有权；peer 只用于日志（形如 "127.0.0.1:51234"）。*/
    MoqLiteEgress(QuicConnection *conn,
                  std::string local_key,   /* MediaStreamManager 里的 key */
                  std::string writer_id,   /* 会话内唯一，RemovePlayer 靠它 */
                  std::string track,       /* 对端看到的 track 名（日志用）*/
                  std::string peer,
                  bool is_video);
    /* ⚠️ 不是 override：`AvWriterInterface` **没有虚析构**（同仓的
     * `MoqPushSession` 也一样没声明）。这不构成泄漏，因为 MediaStreamManager
     * 从不 delete 它持有的 writer —— 摘牌只 erase 指针，对象归会话所有。
     * 前提是**别通过基类指针 delete 本对象**。 */
    ~MoqLiteEgress();

    MoqLiteEgress(const MoqLiteEgress &) = delete;
    MoqLiteEgress &operator=(const MoqLiteEgress &) = delete;

    /* ── AvWriterInterface ── */
    int WritePacket(Media_Packet_Ptr pkt) override;
    std::string GetKey() override { return local_key_; }
    std::string GetWriterId() override { return writer_id_; }
    void CloseWriter() override;
    bool IsInited() override { return init_flag_; }
    void SetInitFlag(bool flag) override { init_flag_ = flag; }

    /* 对端订阅这条轨之后调一次：发 SUBSCRIBE_START{0}，再回放缓存里的 GOP。
     * 回放必然在 SUBSCRIBE_START **之后** —— 那里面报的组号得是诚实的。
     *
     * ⚠️ 两个 id 不是一回事，必须分开传：
     *   subscribe_id   = SUBSCRIBE 消息体里的那个号，**对端选的**，GROUP 头里带的是它
     *   sub_stream_id  = 对端开的那条 Subscribe **bidi 流**的 stream id
     * 曾经只传一个（传的是 stream id）并同时用于两处，于是 GROUP 头报出去的
     * subscribe id 是 stream id：对端从没订阅过那个号，每条组流都被它
     * STOP_SENDING 掉（`quic_stream_on_stop_sending` → reset → 后续 write 全 -1），
     * 症状是「group stream N is dead」刷屏而一帧都播不出来。 */
    void Start(uint64_t subscribe_id, uint64_t sub_stream_id);

    /* 停掉这个订阅：冲掉在途的那一帧、FIN 掉当前组、把订阅流 reset 掉。
     * 幂等。之后 WritePacket 变成空操作（除非重新 Start）。 */
    void Stop(const char *why);

    bool Started() const { return started_; }
    uint64_t Groups() const { return total_groups_; }
    uint64_t Frames() const { return total_frames_; }
    uint64_t Dropped() const { return dropped_groups_; }

private:
    /* 开一条新组（uni 流 + GROUP 头）。拿不到流额度则返回 false。 */
    bool OpenGroup(int64_t ts_us);
    /* 收尾当前组。clean_end = true 时补一个零载荷的结束哨兵帧，**FIN 就搭在
     * 那一帧上**（绝不单独发零长 FIN，原因见 Push 上方的长注释）。 */
    void CloseGroup(bool clean_end);
    /* 对端 reset 掉当前组流：不补哨兵，但**推进组号**。对端按组号去重，
     * 重开旧组号会被忽略（音频实测过“永久没声”）。 */
    void AbandonGroup();
    /* 把一小段编码结果推给当前组流；背压时排进 pending_ 下次重试。
     * `fin=true` 的那一段**不可丢**（丢了组流永远不结束）。 */
    void Push(const uint8_t *data, size_t len, bool fin);
    /* 按序推 pending_ 里积压的段，直到队列空或再撞上水位背压。
     * 某条流的段被对端 reset 掉时**只清那条流**的段，其余继续推。 */
    bool FlushPending();
    /* 组内时间戳（微秒）。pkt 的 dts_ 是毫秒，乘 1000。 */
    int64_t FrameTsUs(Media_Packet_Ptr pkt);
    /* 回放基准：GOP 边缘 30s 窗口内的最小 dts（毫秒）。 */
    void EnsureBase();
    /* 把缓存里的 GOP 回放给这个订阅。 */
    void SendGop();

    void WarnRateLimited(const char *fmt, ...);

    QuicConnection *conn_ = nullptr;
    std::string local_key_;
    std::string writer_id_;
    std::string track_;
    std::string peer_;
    bool is_video_ = false;
    bool init_flag_ = false;
    bool closed_ = false;

    uint64_t subscribe_id_   = 0;  /* 对端选的订阅号 —— GROUP 头里报的是它 */
    uint64_t sub_stream_id_  = 0;  /* 对端那条 Subscribe bidi 流：SUBSCRIBE_START 写这里，
                                    * Stop 时 reset 的也是它 */
    bool     started_ = false;

    /* ── 当前组 ── */
    uint64_t group_sid_ = 0;    /* 0 = 没有开着的组 */
    uint64_t group_seq_ = 0;    /* 组号：本订阅自己从 0 编 */
    uint64_t group_frames_ = 0;
    int64_t  group_prev_us_ = 0;/* 组内上一帧的时间戳；**每组重置为 0**，
                                 * 所以首帧的差 = 它的绝对时间戳 */
    int64_t  group_open_us_ = 0;/* 本组第一帧的时间戳。两个用途：
                                 *  1) 音频按秒切组：ts_us - group_open_us_ >= 1s
                                 *     （**不能**用每帧都更新的 group_prev_us_）；
                                 *  2) 「一个媒体帧都没写进去就收尾」时给哨兵填
                                 *     时间戳，否则哨兵报 0 会把播放头往回拽。 */
    bool     group_had_media_ = false;
    /* 上一帧的通道时间戳（微秒），仅用于诊断：publisher 中断后恢复时打一条
     * “resumed after Nms”，用来区分「服务端没发」和「客户端没播」。 */
    int64_t  last_frame_ts_us_ = -1;

    /* ── 背压待发队列 ──
     * SendEx 返回 1 = **整段未被接受**，原样排到队尾下次再推。为什么是队列而
     * 不是一个槽：回放一个 GOP 是在**一次同步调用**里灌进去几百 KB 的
     * （SendGop 的循环），而这段时间 uv loop 被占着 —— ACK 收不进来、拥塞窗口
     * 不涨，`consume_send_buf` 只能把最初十几 KB 推上线路，其余全积在
     * `send_buf` 里。单个槽的话从水位（512 KiB）往后**每一帧都被丢**，回放出
     * 去的组只剩个开头，迟到者根本接不上（验收标准 ②）。
     * ⚠️ 队列有界。超上限丢**最新**的那段，不丢最旧的：组是「关键帧开头 +
     * 顺序解码」的，丢尾巴留下完整前缀，解码器能播到断点；丢中间是一个洞，
     * 整组作废 —— 比丢尾巴糟得多。 */
    struct PendingSeg {
        uint64_t sid = 0;              /* 这一段属于哪条组流（跨组共存，FIFO 推）*/
        std::vector<uint8_t> data;
        bool fin = false;
    };
    std::deque<PendingSeg> pending_;
    size_t   pending_bytes_ = 0;
    uint64_t dropped_segs_ = 0;        /* 队列满而丢掉的段数（与 dropped_groups_ 分开记）*/

    /* ── 回放时间轴 ── */
    int64_t base_ms_ = -1;
    int64_t last_ts_ms_ = -1;

    uint64_t total_groups_ = 0;
    uint64_t total_frames_ = 0;
    uint64_t dropped_groups_ = 0;
    uint64_t bad_payload_ = 0;  /* 出站自检判为「不是合法 AVCC 形状」的视频帧数。
                                 * 诊断计数器：它 > 0 = 媒体层给我们的就已经是
                                 * 错位的；恒为 0 = 错位发生在更下游。 */
    uint64_t no_credit_ = 0;    /* open_uni 返回 UINT64_MAX 的次数（额度探针）*/
    int64_t  last_warn_s_ = 0;
    int64_t  last_stat_s_ = 0;
};

} /* namespace cpp_streamer */

#endif /* MOQ_LITE_EGRESS_HPP */
