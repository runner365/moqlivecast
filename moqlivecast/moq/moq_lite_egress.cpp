#include "moq/moq_lite_egress.hpp"

#include "moq/moq_lite_wire.hpp"

#include "gop_cache.hpp"
#include "logger.h"
#include "media_stream_manager.hpp"
#include "utils/timeex.hpp"

#include <cstdarg>
#include <cstdio>

namespace cpp_streamer {

namespace {

/* 下行 track 的时间尺度。Legacy 容器的内层前缀恒为**微秒**，外层 FRAME 差值
 * 的单位是 TRACK_INFO 里声明的 timescale —— 两者必须同一个尺度，所以写死
 * 1e6，且这里算出来的是微秒。（与 MoqLiteSession 里的 kServeTimescale 是
 * 同一个约定，那边在 TRACK_INFO 里声明，这边产生数据。） */
constexpr int64_t kUsPerMsLocal = 1000;

/* 音频打包成组的窗口。**不做「一帧一组」**（决策 6 的起步方案是那样）：
 * 组号与组内差值的设计让两种粒度在时间轴上**完全等价**（每组第一帧的差就是
 * 它的绝对时间戳，所以按组累加出来的 ts 不依赖组的切法），差别只在 uni 流的
 * 数量。一帧一组 = 每秒约 45 条新流，而 `quic_stream_open_uni` 一旦耗尽就
 * 只能丢组（丢一组音频 = 一次可听的断点）。按秒打包把 churn 直接降 45 倍，
 * 且不引入任何缓冲延迟 —— 帧是**边到边写**的，组边界不影响投递时机。
 * 视频侧不受影响（视频天然一 GOP 一组，是硬要求不是选择）。 */
constexpr int64_t kAudioGroupUs = 1000000;

/* 回放基准的「GOP 边缘窗口」：只用末尾 30s 内的包定原点，否则残留的旧低 dts
 * 会把原点拉到几十万，首帧时间戳就没意义了（照抄 moq_push.cpp 的实测值）。 */
constexpr int64_t kGopEdgeWindowMs = 30000;

/* 相对时间戳一次跳超过这个值就重锚。 */
constexpr int64_t kRebaseJumpMs = 5000;

/* 音视频时钟错位判定：音频已跑到视频前面这么多毫秒，说明缓存的这个 GOP 已经
 * 陈旧（视频还没跟上），直接跳过回放等下一个关键帧。 */
constexpr int64_t kStaleGopMs = 400;

constexpr uint64_t kStatEveryGroups = 100;

/* 待发队列上限。必须装得下**一个完整回放 GOP**：实测 3.8s 的 GOP 是 243 个
 * 视频包，回放循环里绝大部分要排队（见 hpp 里的成员注释）。2 MiB 对
 * ~700kbps 的源是 20 秒的量，够用且不到单连接内存预算的量级问题。
 * 上限本身不引入延迟：帧是边到边写的，队列只在背压时变长。 */
constexpr size_t kPendingCapBytes = 2 * 1024 * 1024;

} /* namespace */

MoqLiteEgress::MoqLiteEgress(QuicConnection *conn, std::string local_key,
                             std::string writer_id, std::string track,
                             std::string peer, bool is_video)
    : conn_(conn),
      local_key_(std::move(local_key)),
      writer_id_(std::move(writer_id)),
      track_(std::move(track)),
      peer_(std::move(peer)),
      is_video_(is_video) {}

MoqLiteEgress::~MoqLiteEgress() {
    /* 不在这里 RemovePlayer：调用方（会话）负责摘牌，析构时媒体层可能已经在
     * 拆了。这里只保证不再往 QUIC 上写。 */
    started_ = false;
    closed_ = true;
}

void MoqLiteEgress::WarnRateLimited(const char *fmt, ...) {
    const int64_t now_s = now_millisec() / 1000;
    if (now_s == last_warn_s_) return;
    last_warn_s_ = now_s;
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    LOG_WARN("[moq-lite] egress %s track=\"%s\" key=\"%s\": %s",
             peer_.c_str(), track_.c_str(), local_key_.c_str(), buf);
}

void MoqLiteEgress::Start(uint64_t subscribe_id, uint64_t sub_stream_id) {
    if (closed_ || started_) return;
    if (sub_stream_id == 0) return;

    std::vector<uint8_t> out;
    /* SUBSCRIBE_START{group=0}：**接受订阅是隐式的**，这条就是第一条应答，
     * 没有 SUBSCRIBE_OK（lite/subscribe.rs:490-518）。组号必须是诚实的 ——
     * 本实例的组号从 0 起编，所以这里报 0。
     * fin=0：订阅流要一直开着（将来发 SUBSCRIBE_END 也走它）。 */
    if (!MlBuildSubscribeStart(out, 0)) {
        LOG_ERROR("[moq-lite] egress %s track=\"%s\": cannot encode SUBSCRIBE_START",
                  peer_.c_str(), track_.c_str());
        return;
    }
    if (QuicConnectionStreamSend(conn_, sub_stream_id, out.data(), out.size(), 0) < 0) {
        LOG_ERROR("[moq-lite] egress %s track=\"%s\": cannot send SUBSCRIBE_START "
                  "on stream %llu",
                  peer_.c_str(), track_.c_str(),
                  static_cast<unsigned long long>(sub_stream_id));
        return;
    }

    subscribe_id_  = subscribe_id;
    sub_stream_id_ = sub_stream_id;
    started_ = true;
    LOG_INFO("[moq-lite] egress %s track=\"%s\" key=\"%s\": SUBSCRIBE_START group=0, "
             "replaying cached GOP (subscribe id=%llu on stream=%llu)",
             peer_.c_str(), track_.c_str(), local_key_.c_str(),
             static_cast<unsigned long long>(subscribe_id_),
             static_cast<unsigned long long>(sub_stream_id_));
    SendGop();
}

void MoqLiteEgress::Stop(const char *why) {
    if (closed_) return;
    closed_ = true;
    started_ = false;
    /* 收尾：在途的一帧不再重试，当前组不补哨兵（它是被**拆掉**的，补一个带
     * 时间戳的空帧只会让对端多一个畸形帧 —— 直接 FIN 就是干净的结束）。*/
    pending_.clear();
    pending_bytes_ = 0;
    if (group_sid_ != 0) {
        /* fin=1 且 len=0：给这条 uni 流收尾，不写新数据。
         * ⚠️ **尽力而为，不保证送达**（见 Push 上方的长注释）：send_buf 那一刻
         * 已经排空的话这次调用是空操作。这里可以接受 —— Stop 是订阅被拆掉，
         * 下面马上就把订阅流 reset 了，对端不会再读这条组流的尾巴；正常收尾
         * 那条路（CloseGroup）才是必须成功的，它走哨兵帧。 */
        if (QuicConnectionStreamSendEx(conn_, group_sid_, nullptr, 0, 1,
                                       nullptr, nullptr, 0) < 0) {
            LOG_DEBUG("[moq-lite] egress %s track=\"%s\": close group stream %llu "
                      "failed (already gone)",
                      peer_.c_str(), track_.c_str(),
                      static_cast<unsigned long long>(group_sid_));
        }
        group_sid_ = 0;
    }
    if (sub_stream_id_ != 0) {
        QuicConnectionStreamReset(conn_, sub_stream_id_, 0x1 /* CANCELLED */);
        sub_stream_id_ = 0;
    }
    LOG_INFO("[moq-lite] egress %s track=\"%s\" key=\"%s\": stopped (%s), "
             "groups=%llu frames=%llu dropped=%llu dropped_frames=%llu "
             "no_credit=%llu bad_payload=%llu",
             peer_.c_str(), track_.c_str(), local_key_.c_str(), why ? why : "",
             (unsigned long long)total_groups_,
             (unsigned long long)total_frames_,
             (unsigned long long)dropped_groups_,
             (unsigned long long)dropped_segs_,
             (unsigned long long)no_credit_,
             (unsigned long long)bad_payload_);
}

void MoqLiteEgress::CloseWriter() {
    Stop("writer closed by media manager");
}

bool MoqLiteEgress::OpenGroup(int64_t ts_us) {
    const uint64_t sid = QuicConnectionStreamOpenUni(conn_);
    if (sid == UINT64_MAX) {
        /* 额度耗尽。**不得阻塞**：这条路径在全进程唯一的 uv loop 线程上，
         * sleep/spin 会拖死所有连接。open_uni 耗尽时不会推进 next_local_uni_id，
         * 所以下次重试是安全的（quic_stream.c:132-156）。
         * 处理：整组丢掉（连同它后面的帧，直到下一个关键帧），由对端看到一个
         * 可见的空洞 —— 与「丢最旧的组」的用户可见结果相同，但**零缓冲**，
         * 不存在无界队列把内存拖爆这条路径。 */
        no_credit_++;
        dropped_groups_++;
        WarnRateLimited("no uni stream credit (open_uni exhausted), dropping group "
                        "seq=%llu; dropped_total=%llu no_credit=%llu",
                        (unsigned long long)group_seq_,
                        (unsigned long long)dropped_groups_,
                        (unsigned long long)no_credit_);
        return false;
    }

    std::vector<uint8_t> out;
    /* uni 流的头是 STREAM_TYPE(=0x0 Group)，然后才是 GROUP 消息。
     * 头里的 subscribe id 必须是**对端在 SUBSCRIBE 里选的那个号**（subscribe_id_），
     * 不是这条 bidi 流的 stream id —— 报错了对端会把整条组流 STOP_SENDING 掉。 */
    MlAppendVarint(out, 0x0);
    if (!MlBuildGroupHeader(out, subscribe_id_, group_seq_, /*frame_start=*/0)) {
        QuicConnectionStreamReset(conn_, sid, 0x0);
        LOG_ERROR("[moq-lite] egress %s track=\"%s\": cannot encode GROUP header",
                  peer_.c_str(), track_.c_str());
        return false;
    }
    if (QuicConnectionStreamSendEx(conn_, sid, out.data(), out.size(), 0,
                                   nullptr, nullptr, 0) < 0) {
        LOG_ERROR("[moq-lite] egress %s track=\"%s\": cannot write GROUP header "
                  "on stream %llu",
                  peer_.c_str(), track_.c_str(),
                  static_cast<unsigned long long>(sid));
        return false;
    }

    group_sid_ = sid;
    group_frames_ = 0;
    group_prev_us_ = 0;
    group_open_us_ = ts_us;
    group_had_media_ = false;
    return true;
}

/* 对端 reset 掉当前组流：不补哨兵（流已经没了），但**必须推进组号**。
 * 对端按 (subscribe id, group seq) 去重；重开同一个组号会被当成重复忽略。
 * 音频实测踩过：组流被 reset 后一直重开 0 号组，播放端从此没有声音。 */
void MoqLiteEgress::AbandonGroup() {
    if (group_sid_ == 0) return;
    group_sid_ = 0;
    group_frames_ = 0;
    group_prev_us_ = 0;
    group_open_us_ = 0;
    group_had_media_ = false;
    group_seq_++;
}

void MoqLiteEgress::CloseGroup(bool clean_end) {
    if (group_sid_ == 0) return;

    /* 组的结束哨兵 —— **FIN 就搭在这一帧上**，见下面 Push 上方的长注释。
     *
     * 线上形状（M3 侧实测踩过这个坑）：payload 不是空的，而是「内层微秒
     * varint + **零字节**编解码载荷」；对端解出时间戳之后发现载荷为空，才认成
     * 哨兵而不是一个要送去解码的帧。它记的是**组的结束时刻**，正好等于本组
     * 最后一帧的时间戳 —— 所以差值相对上一帧取 0。
     *
     * ⚠️ **音频也写哨兵**。（早先的版本只给视频写，注释里写「音频不写，靠组
     * FIN 本身结束」—— 那是建立在「零长 FIN 发得出去」这个错误前提上的，见
     * Push。音频没有可搭 FIN 的载荷，不补哨兵就只能退回那个发不出去的写法。）
     * 音频哨兵在线格式上是**合法**的：`legacy::Wire::end()` 判的是
     * `kind != Data && payload.is_empty()`（legacy/mod.rs:20-22），Audio 不是
     * Data，所以对端把它当组结束标记，不会当成一个空音频帧丢给解码器。
     * 只有 catalog 那条轨是 Kind::Data —— 它不走本类。 */
    bool fin_attached = false;
    if (clean_end) {
        /* 一个媒体帧都没写进去的退化路径用开组时间戳，**不能用 0**：哨兵时间
         * 戳会被对端当成组的结束时刻，报 0 会把播放头往回拽。 */
        const int64_t end_us = group_had_media_ ? group_prev_us_ : group_open_us_;
        std::vector<uint8_t> payload;
        if (MlWriteVarint(payload, static_cast<uint64_t>(end_us))) {
            std::vector<uint8_t> seg;
            if (MlAppendFrame(seg, 0, payload.data(), payload.size())) {
                Push(seg.data(), seg.size(), true);   /* ← FIN 在这里 */
                fin_attached = true;
            }
        }
    }
    /* 兜底：哨兵编不出来（不该发生）时退回零长 FIN。它在 send_buf 还没排空时
     * 是有效的，排空了就是空操作 —— 所以正常路径绝不走这里。 */
    if (!fin_attached) Push(nullptr, 0, true);
    /* ⚠️ 这里**不再**有「FIN 没发出去就 reset 兜底」那一段：队列里可能还排着
     * 刚回放出去的几百 KB，reset 一条组流等于把它们全扔掉。 */

    LOG_DEBUG("[moq-lite] egress %s track=\"%s\": group seq=%llu frames=%llu closed",
              peer_.c_str(), track_.c_str(),
              static_cast<unsigned long long>(group_seq_),
              static_cast<unsigned long long>(group_frames_));
    total_groups_++;
    group_sid_ = 0;
    group_frames_ = 0;
    group_prev_us_ = 0;
    group_open_us_ = 0;
    group_had_media_ = false;
    group_seq_++;

    if (total_groups_ % kStatEveryGroups == 0) {
        LOG_INFO("[moq-lite] egress %s track=\"%s\" key=\"%s\": groups=%llu "
                 "frames=%llu dropped=%llu dropped_frames=%llu queued=%zu "
                 "no_credit=%llu bad_payload=%llu",
                 peer_.c_str(), track_.c_str(), local_key_.c_str(),
                 (unsigned long long)total_groups_,
                 (unsigned long long)total_frames_,
                 (unsigned long long)dropped_groups_,
                 (unsigned long long)dropped_segs_,
                 pending_bytes_,
                 (unsigned long long)no_credit_,
                 (unsigned long long)bad_payload_);
    }
}

/* ⚠️⚠️ 关于 FIN —— 这一条是**踩过的坑**，改代码前先读完。
 *
 * **零长 + fin=1 的写请求在 `send_buf` 已排空时会被静默丢弃。**
 * `quic_stream_write`（quic_stream.c:392-429）对 len==0 只做 `s->send_buf_fin = 1`
 * 然后就进 `consume_send_buf`；后者的循环条件正是 `while (s->send_buf_len > 0)`，
 * 缓冲空时一次都不进，直接落到收尾的 `s->send_buf_fin = 0`（:356-361）——
 * 刚设上的 FIN 被自己清掉，而 on-wire 的 STREAM 帧一帧都没发。函数照样 return 0，
 * 上层**看不出任何异常**。
 *
 * 后果不是「少个字节」而是**组永远不结束**：对端 `moq-mux` 的 `Consumer` 只在
 * `format.poll_read` 返回 `None`（= 读到组流结尾）时才 `pop_front(); current += 1`
 * 推进到下一组（container/consumer.rs 的 `Poll::Ready(Ok(None))` 分支）。组流不
 * FIN，它就一直停在那组上，靠时间预算跳组 —— 丢帧，而且丢得没有规律。
 *
 * 所以本类的铁律：**FIN 一律搭在一个非空的数据段上**（`CloseGroup` 的哨兵帧），
 * 或者在一个 `send_buf` 还没排空的时刻发（那时 `while` 会进去，`is_last` 才真
 * 正被写进 STREAM 帧）。任何「单独发一个零长 FIN 来收尾」的写法都是错的。 */
void MoqLiteEgress::Push(const uint8_t *data, size_t len, bool fin) {
    if (group_sid_ == 0) return;

    /* 积压没排空就**绝不插队** —— 组内字节顺序颠倒就是花屏。先尽力推一把。 */
    if (!pending_.empty()) FlushPending();

    if (pending_.empty() && group_sid_ != 0) {
        /* 队列空着：直接试写，省一次拷贝（常态就是走这条）。 */
        const int r = QuicConnectionStreamSendEx(conn_, group_sid_, data, len, fin ? 1 : 0,
                                                 nullptr, nullptr, 0);
        if (r == 0) return;
        if (r < 0) {
            /* 流已废（对端 reset / 已 FIN 过）。本组作废，等下一个关键帧重开。 */
            WarnRateLimited("group stream %llu is dead, abandoning group seq=%llu",
                            (unsigned long long)group_sid_,
                            (unsigned long long)group_seq_);
            AbandonGroup();
            return;
        }
        /* r == 1：整段未被接受，落到下面排队。 */
    } else if (group_sid_ == 0) {
        return;      /* 排空积压时把当前组流作废了（对端 reset），这段没处可去 */
    }

    /* 排队。**带 FIN 的段不可丢**（丢了下游那组就永远不结束，见上面的长注释）；
     * 它只可能是收尾哨兵，十来字节，对上限没有影响。 */
    if (len > 0 && !fin && pending_bytes_ + len > kPendingCapBytes) {
        WarnRateLimited("send queue full (%zu bytes queued), dropping the newest "
                        "%zu-byte segment of group seq=%llu; dropped_frames_total=%llu",
                        pending_bytes_, len,
                        (unsigned long long)group_seq_,
                        (unsigned long long)dropped_segs_);
        dropped_segs_++;
        return;
    }

    PendingSeg seg;
    seg.sid = group_sid_;
    seg.fin = fin;
    if (len > 0) seg.data.assign(data, data + len);
    pending_bytes_ += len;
    pending_.push_back(std::move(seg));
}

bool MoqLiteEgress::FlushPending() {
    while (!pending_.empty()) {
        const uint64_t sid  = pending_.front().sid;
        const size_t   size = pending_.front().data.size();
        const bool     fin  = pending_.front().fin;
        const uint8_t *data = pending_.front().data.empty()
                            ? nullptr : pending_.front().data.data();

        const int r = QuicConnectionStreamSendEx(conn_, sid, data, size, fin ? 1 : 0,
                                                 nullptr, nullptr, 0);
        if (r == 1) return true;   /* 水位背压：整段留着，下次再推 */

        if (r < 0) {
            /* 队首那条流废了（对端 reset / 已 FIN）。**同一条流**上排在它后面的
             * 段也没有意义了，一起清掉；别的流的段继续推。 */
            while (!pending_.empty() && pending_.front().sid == sid) {
                pending_bytes_ -= pending_.front().data.size();
                pending_.pop_front();
            }
            if (group_sid_ == sid) AbandonGroup();
            continue;
        }

        pending_bytes_ -= size;
        pending_.pop_front();
    }
    return true;
}

void MoqLiteEgress::EnsureBase() {
    if (base_ms_ >= 0) return;
    GopCache *gop = MediaStreamManager::GetGop(local_key_);
    if (!gop) return;

    int64_t last_v = -1;
    for (const auto &p : gop->Packets()) {
        if (p && p->av_type_ == MEDIA_VIDEO_TYPE && !p->is_seq_hdr_ && p->dts_ > 0) {
            last_v = p->dts_;
        }
    }
    int64_t origin = -1;
    for (const auto &p : gop->Packets()) {
        if (!p || p->is_seq_hdr_ || p->dts_ <= 0) continue;
        if (last_v >= 0 && p->av_type_ == MEDIA_AUDIO_TYPE && p->dts_ > last_v + 40) {
            continue;
        }
        if (last_v >= 0 && p->dts_ + kGopEdgeWindowMs < last_v) continue;
        if (origin < 0 || p->dts_ < origin) origin = p->dts_;
    }
    if (origin < 0 && last_v >= 0) origin = last_v;
    if (origin < 0) return;
    base_ms_ = origin;
    LOG_INFO("[moq-lite] egress %s track=\"%s\": replay ts_base=%lld "
             "(gop edge window, last_v=%lld)",
             peer_.c_str(), track_.c_str(), (long long)base_ms_, (long long)last_v);
}

int64_t MoqLiteEgress::FrameTsUs(Media_Packet_Ptr pkt) {
    if (!pkt) return 0;
    const int64_t dts = pkt->dts_ < 0 ? 0 : pkt->dts_;

    if (base_ms_ < 0) {
        if (dts <= 0) return 0;
        base_ms_ = dts;
        LOG_INFO("[moq-lite] egress %s track=\"%s\": pull ts_base=%lld",
                 peer_.c_str(), track_.c_str(), (long long)base_ms_);
    }
    int64_t ts = dts - base_ms_;
    if (ts < 0) ts = 0;

    if (last_ts_ms_ < 0 && ts > kRebaseJumpMs) {
        /* 首个媒体包离原点就很远（缓存里没有可用的 GOP → base 是旧值）：
         * 直接从 0 起播，否则对端首帧就在几分钟处。 */
        LOG_INFO("[moq-lite] egress %s track=\"%s\": pull ts rebase first "
                 "dts=%lld old_base=%lld -> 0",
                 peer_.c_str(), track_.c_str(), (long long)dts, (long long)base_ms_);
        base_ms_ = dts;
        ts = 0;
    } else if (last_ts_ms_ >= 0 && ts > last_ts_ms_ + kRebaseJumpMs) {
        /* 时间轴一次跳了几分钟：相对 ts 重锚到上一包附近。重锚后组内已编码的
         * 旧 ts 与新时间轴混在一组里会让对端 rewind —— 把当前组拆掉。 */
        const int64_t old_base = base_ms_;
        base_ms_ = dts - (last_ts_ms_ + 40);
        ts = dts - base_ms_;
        if (ts < 0) ts = 0;
        LOG_INFO("[moq-lite] egress %s track=\"%s\": pull ts rebase old_base=%lld "
                 "new_base=%lld dts=%lld",
                 peer_.c_str(), track_.c_str(), (long long)old_base,
                 (long long)base_ms_, (long long)dts);
        /* clean_end = false：**故意不补哨兵**。这一组的帧都在旧时间轴上，补一个
         * 「组在这里结束」的标记等于把对端的播放头钉在旧时间轴的末端，而下一组
         * 的帧会从重锚后的 0 附近重新开始 —— 那是解法比问题更糟的一处。
         * ⚠️ 代价要说清楚：这样收尾就只剩 CloseGroup 里那个**零长 FIN 兜底**，
         * 而它在 send_buf 已排空时是空操作（见 Push 上方的长注释），也就是这条
         * 退化路径上组流**可能不被 FIN**，对端要等时间预算跳过它。重锚本身
         * 已经是一次时间倒流，对端那组怎么都要经历一次异常，所以这里选「不
         * 加戏」。这条路只在源时钟跳变（重推/换源）时才走到，静态文件源不会。 */
        CloseGroup(false);
    }
    last_ts_ms_ = ts;
    return ts * kUsPerMsLocal;
}

void MoqLiteEgress::SendGop() {
    GopCache *gop = MediaStreamManager::GetGop(local_key_);
    if (!gop) {
        LOG_INFO("[moq-lite] egress %s track=\"%s\": no gop, wait for live",
                 peer_.c_str(), track_.c_str());
        return;
    }
    EnsureBase();

    const int64_t want_type = is_video_ ? MEDIA_VIDEO_TYPE : MEDIA_AUDIO_TYPE;
    int64_t last_video_dts = -1;
    int64_t last_audio_dts = -1;
    for (const auto &p : gop->Packets()) {
        if (!p || p->is_seq_hdr_) continue;
        if (p->av_type_ == MEDIA_VIDEO_TYPE) last_video_dts = p->dts_;
        if (p->av_type_ == MEDIA_AUDIO_TYPE) last_audio_dts = p->dts_;
    }
    /* 音频已经跑到视频前面很远 = 缓存里这个 GOP 相对音频已经陈旧，视频回放
     * 出去只会让对端先看到一段旧画面。跳过回放，等下一个关键帧（那时
     * WritePacket 会自己开新组）。音频侧不受影响。 */
    if (is_video_ && last_video_dts >= 0 && last_audio_dts >= 0 &&
        last_audio_dts - last_video_dts > kStaleGopMs) {
        LOG_INFO("[moq-lite] egress %s track=\"%s\": skip stale gop "
                 "video_end=%lld audio_end=%lld, wait for a live keyframe",
                 peer_.c_str(), track_.c_str(),
                 (long long)last_video_dts, (long long)last_audio_dts);
        return;
    }

    size_t sent = 0;
    for (const auto &pkt : gop->Packets()) {
        if (!pkt || pkt->is_seq_hdr_) continue;
        if (pkt->av_type_ != want_type) continue;
        /* 音频不要越过视频的末尾：越过的那部分属于下一段，会和马上要到来的
         * 直播包重复（照抄 moq_push.cpp 的实测守卫）。 */
        if (!is_video_ && last_video_dts >= 0 && pkt->dts_ > last_video_dts + 40) {
            continue;
        }
        if (WritePacket(pkt) < 0) break;
        sent++;
    }
    LOG_INFO("[moq-lite] egress %s track=\"%s\": replayed gop packets=%zu "
             "(cached=%zu) groups=%llu frames=%llu",
             peer_.c_str(), track_.c_str(), sent, gop->Packets().size(),
             (unsigned long long)total_groups_,
             (unsigned long long)total_frames_);
}

int MoqLiteEgress::WritePacket(Media_Packet_Ptr pkt) {
    if (closed_ || !started_ || !pkt) return 0;
    if (!pkt->buffer_ptr_ || pkt->buffer_ptr_->DataLen() == 0) return 0;
    /* 序列头**不进组**：它是由 catalog 合成出来的 FLV 头，对端从 catalog 拿
     * 编解码参数，收到一个「头」当媒体帧只会让解码器困惑。 */
    if (pkt->is_seq_hdr_) return 0;

    /* ── 只处理本轨的包 ──
     * 本类是 MediaStreamManager 的一个 **player**，那边的扇出
     * （media_stream_manager.cpp:422）把这条 stream 上的**每一个**包都交给
     * 注册过的**每一个** writer —— 而每条被订阅的轨各有一个本类实例。于是
     * 视频写入器会收到音频包、音频写入器会收到视频包，**两条交叉串**。
     *
     * ⚠️ 不过滤的后果不是「多发了一点」而是**静默错位**：下面 tag_hdr 是按
     * **轨**（is_video_）定的，不是按包定的。音频包进了视频写入器 → 从 2 字节
     * 的音频 tag 头里砍掉 5 字节 → AAC 的第 3 字节被当成 AVCC 长度；反过来
     * 视频包进了音频写入器 → 只砍 2 字节 → 残留 `[AVCPacketType][CTS 00 00]`
     * 被喂给 AAC 解码器。两侧都不报错，只在**对端解码器**里炸：
     *   ffmpeg: Invalid NAL unit size (-263378 > 161)   ← 0xFFFBFB2E 按 int32 读
     *           Prediction is not allowed in AAC-LC. / Reserved bit set.
     * 服务端侧唯一可见的症状是 bad_payload 计数（`first NAL length=-263378 …`）。
     *
     * 判据取自**包自己**的 av_type_，与其它 writer 一致（rtmp_writer.cpp:28、
     * httpflv_writer.cpp:72 都是这个写法）。返回 0 而**不是 -1** —— 这是正常
     * 情况，-1 会让 MediaStreamManager 把本 writer 摘牌。
     *
     * SendGop（回放那条路）本来就有等价的过滤（want_type），只有这条 live
     * 路径漏了，所以 M2/M3 自测才一直没暴露出来。 */
    /* ⚠️ 必须是**显式白名单**，不能写成 `(av_type == VIDEO) != is_video`。
     * 后者会把第三种类型（MEDIA_METADATA_TYPE，RTMP 的 onMetaData）当成
     * “非视频”收进**音频** writer，然后按音频 tag 头剥 2 字节当 AAC 发出去，
     * 对端解码器会报 Reserved bit set / 噪声。WebTransport 与 HTTP-FLV/RTMP
     * writer 各自都有 metadata 分支，只有这条 egress 需要在这里挡掉。 */
    const bool want_type = is_video_ ? (pkt->av_type_ == MEDIA_VIDEO_TYPE)
                                     : (pkt->av_type_ == MEDIA_AUDIO_TYPE);
    if (!want_type) return 0;

    const int64_t ts_us = FrameTsUs(pkt);
    const bool key = pkt->is_key_frame_;

    /* 诊断：publisher 中断/恢复（重推、切源）之后，音频是不是还在发。
     * 只打第一条恢复帧，稳态零开销。 */
    if (last_frame_ts_us_ >= 0 && ts_us - last_frame_ts_us_ > 500000) {
        LOG_INFO("[moq-lite] egress %s track=\"%s\": media resumed after %lldms "
                 "(ts_us=%lld group=%llu seq=%llu)",
                 peer_.c_str(), track_.c_str(),
                 (long long)((ts_us - last_frame_ts_us_) / 1000),
                 (long long)ts_us,
                 (unsigned long long)group_sid_,
                 (unsigned long long)group_seq_);
    }
    last_frame_ts_us_ = ts_us;

    if (is_video_) {
        if (key) {
            /* 关键帧 = 组边界。这是**硬要求**：Legacy/LOC 线上没有关键帧标志位，
             * 消费者靠「组内第一帧即关键帧」这个协议不变量补 —— 组不是在关键帧
             * 上开的，对端就是花屏。 */
            if (group_sid_ != 0) CloseGroup(true);
            if (!OpenGroup(ts_us)) return 0;   /* 没额度：整组连同后续帧丢掉 */
        } else if (group_sid_ == 0) {
            /* 还没等到关键帧（回放被跳过、或刚重锚）。丢掉，等关键帧。 */
            return 0;
        }
    } else {
        /* 音频：按秒打包。到点了就先收尾再开新组。
         *
         * ⚠️ 必须和 **group_open_us_**（本组第一帧的时间）比，不能和
         * group_prev_us_（**上一帧**的时间，每帧都更新）比 —— 后者算出来永远
         * 只有一帧的间隔（~21ms），永远到不了 1 秒，整条流会变成一个几分钟长
         * 的大组。实测对端最终会把那条组流 reset 掉，而下面 AbandonGroup 之前
         * 没有推进组号，重开的组还是 0 号，被对端按组号去重忽略 → **永久没声**。
         * ⚠️ clean_end = **true** —— 音频也要那个收尾哨兵，因为 FIN 得搭在
         * 非空段上才发得出去（见 Push 上方的长注释）。 */
        /* 时间戳回退也要换组：publisher 重启时共享时钟可能让音频比上一帧
         * 早几十毫秒。若继续追加到旧组，组内会出现**负的 frame delta** ——
         * 对端的 group consumer 有理由把它判成畸形/中止。新组的首帧差值是
         * 绝对时间戳，永远是正的。 */
        if (group_sid_ != 0 &&
            ((ts_us - group_open_us_) >= kAudioGroupUs || ts_us < group_prev_us_)) {
            CloseGroup(true);
        }
        if (group_sid_ == 0 && !OpenGroup(ts_us)) return 0;
    }

    /* ⚠️⚠️ `buffer_ptr_` 装的是 **FLV tag body**（含 tag 自己的头），不是裸编解码
     * 字节。MediaStreamManager 里所有包都是这个形状（`Media_Packet::fmt_type_`
     * = FLV），而 Legacy 容器要的是**剥掉 tag 头之后**的裸载荷：
     *   视频 tag body = [FrameType|CodecID][AVCPacketType][CTS×3] + AVCC NALU
     *   音频 tag body = [SoundFormat|…][AACPacketType]           + 裸 AAC
     * 不剥的后果不是「差一点」而是**对端整帧解不开**：这 5（或 2）个字节会被
     * 当成 AVCC 的第一个 NAL 长度前缀 —— 实测 ffplay 报
     *   Invalid NAL unit size (654376960 > 4103)
     * 而 654376960 = 0x27010000，正是 `[0x27=inter|AVC][0x01=NALU][CTS 前两字节 00 00]`。
     * 参考实现同一条路就是这么剥的：moq_push.cpp:867 `payload = body + 5`
     * / `body + 2`（浏览器那条线一直在剥，所以那边是对的）。
     * CTS 丢掉是**协议使然**，不是遗漏：Legacy 的 hang::container::Frame 只有
     * {timestamp, payload} 两个字段，没有 composition time 可放（收侧同理，
     * 见 MoqLiteSession::OnMediaFrame 的注释）。
     * ⚠️ 非 AAC 音频（MP3）的 tag 头只有 1 字节，这里不支持 —— 与 EmitLoc 的
     * 取舍一致，本轮只服务我们自己 moq-lite 推上来的流（恒 AAC）。 */
    const auto *body = reinterpret_cast<const uint8_t *>(pkt->buffer_ptr_->Data());
    const size_t body_len = pkt->buffer_ptr_->DataLen();
    const size_t tag_hdr = is_video_ ? 5 : 2;
    if (body_len <= tag_hdr) return 0;
    const uint8_t *codec = body + tag_hdr;
    const size_t codec_len = body_len - tag_hdr;

    /* ── 出站自检（**只诊断，不改行为**）──
     * 视频帧的裸载荷必须是 AVCC 形状：`[4 字节大端长度][NALU]…`。这里只查
     * **第一个** NAL 长度 —— 错位时它最能说明问题。对端 ffmpeg 的报错长这样：
     *     Invalid NAL unit size (-454 > 165)
     * 第二个数 = 帧长 − 4，第一个数是帧首那 4 字节按**有符号** int32 读出来的。
     * 所以判据用**无符号**读：合法时它在 (0, codec_len − 4] 内（帧由单个 NAL
     * 组成时正好等于 codec_len − 4）；而任何错位字节（例如把 FLV tag 头
     * `27 01 00 00` 当成长度 = 654376960）都远大于帧长，一撞就出来。
     *
     * 这个计数器的用途是**把责任分层**：bad_payload 恒为 0 → 我们送出去的
     * 字节是对的，错位在更下游（对端/传输）；一旦 > 0 → 媒体层给我们的就已经
     * 是错位的，别再去查 moq-lite 这条路。 */
    if (is_video_ && codec_len > 4) {
        const uint32_t nal_len = (static_cast<uint32_t>(codec[0]) << 24) |
                                 (static_cast<uint32_t>(codec[1]) << 16) |
                                 (static_cast<uint32_t>(codec[2]) << 8) |
                                 static_cast<uint32_t>(codec[3]);
        if (nal_len == 0 || static_cast<size_t>(nal_len) + 4 > codec_len) {
            bad_payload_++;
            WarnRateLimited("outgoing video payload is not AVCC-shaped: first NAL "
                            "length=%ld (as int32) len=%zu head=%02x%02x%02x%02x "
                            "%02x%02x%02x%02x bad_total=%llu",
                            static_cast<long>(static_cast<int32_t>(nal_len)),
                            codec_len,
                            codec[0], codec[1], codec[2], codec[3],
                            codec_len > 8 ? codec[4] : 0, codec_len > 9 ? codec[5] : 0,
                            codec_len > 10 ? codec[6] : 0, codec_len > 11 ? codec[7] : 0,
                            static_cast<unsigned long long>(bad_payload_));
        }
    }

    /* 帧体 = [FRAME: zigzag 差值][Message Length][payload]，payload 是
     * Legacy 容器的第二层：`[绝对微秒 varint][裸编解码字节]`。
     * 差值**每组相对 0 起算**（group_prev_us_ 在 OpenGroup 里清 0），所以本组
     * 第一帧的差就是它的绝对时间戳 —— 对端按组累加就能还原整条时间轴。 */
    std::vector<uint8_t> payload;
    payload.reserve(codec_len + 8);
    if (!MlWriteVarint(payload, static_cast<uint64_t>(ts_us))) return 0;
    payload.insert(payload.end(), codec, codec + codec_len);

    std::vector<uint8_t> seg;
    seg.reserve(payload.size() + 16);
    if (!MlAppendFrame(seg, ts_us - group_prev_us_, payload.data(), payload.size())) {
        WarnRateLimited("frame timestamp delta out of zigzag range, dropping");
        return 0;
    }

    group_prev_us_ = ts_us;
    group_frames_++;
    group_had_media_ = true;
    total_frames_++;
    Push(seg.data(), seg.size(), false);
    return 0;
}

} /* namespace cpp_streamer */
