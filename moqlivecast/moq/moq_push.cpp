#include "moq/moq_push.hpp"
#include "moq/moq_varint.hpp"
#include "moq_handler.hpp"
#include "media_packet.hpp"
#include "media_stream_manager.hpp"
#include "av.hpp"
#include "flv_pub.hpp"
#include "utils/meta_log.hpp"
#include "logger.h"
#include "utils/timeex.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

namespace cpp_streamer {

namespace {

constexpr uint64_t kMoqtSetup = 0x2f00;
constexpr uint64_t kMoqtPublish = 0x1d;
constexpr uint64_t kMoqtSubscribe = 0x03;
constexpr uint64_t kMoqtSubscribeOk = 0x04;
/* GOAWAY 是控制流上的消息之一（draft §10.4，type=0x10）。
 * 与 SETUP 同属控制流；PUBLISH/SUBSCRIBE 则各自占一条请求双向流。 */
constexpr uint64_t kMoqtGoaway = 0x10;
constexpr uint64_t kLocTimescale = 0x08;
constexpr uint64_t kLocFrameMark = 0x09;
constexpr uint64_t kLocVideoConfig = 0x0d;
constexpr uint64_t kLocCompositionTime = 0x0e;  /* 私有扩展：pts - dts */
constexpr uint64_t kLocAudioConfig = 0x0f;
constexpr uint64_t kLocTimestamp = 0x10;
constexpr uint64_t kAliasVideo = 1;
constexpr uint64_t kAliasAudio = 2;

bool ReadBytes(const uint8_t *p, size_t len, size_t &off,
               size_t n, const uint8_t **out) {
    if (off + n > len) return false;
    *out = p + off;
    off += n;
    return true;
}

void LogFirstBytes(const uint8_t *p, size_t len) {
    char hex[3 * 16 + 1] = {0};
    const size_t n = len < 16 ? len : 16;
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        o += static_cast<size_t>(std::snprintf(hex + o, sizeof(hex) - o,
                                               "%02x ", p[i]));
    }
    LOG_INFO("[moq-rfc] stream first %zuB hex=%s", len, hex);
}

const char *ClassifyFirstType(uint64_t type, const uint8_t *p, size_t len) {
    if (len >= 3 && p[0] == 'F' && p[1] == 'L' && p[2] == 'V') {
        return "FLV magic (client sent FLV, not MOQT SETUP/PUBLISH)";
    }
    if (type == kMoqtSetup) return "SETUP";
    if (type == kMoqtPublish) return "PUBLISH";
    if (type == kMoqtSubscribe) return "SUBSCRIBE";
    if (type == kMoqtSubscribeOk) return "SUBSCRIBE_OK";
    if (type == 0) return "bind catalog";
    if (type == kAliasVideo) return "bind video";
    if (type == kAliasAudio) return "bind audio";
    if (type == 8) return "FLV audio tag type=8";
    if (type == 9) return "FLV video tag type=9";
    if (type == 18) return "FLV script tag type=18";
    if (type == 1612) return "2-byte varint of 'FL' (FLV header)";
    if ((type & 0x10) != 0) return "SUBGROUP/data flags";
    return "unknown";
}

void AppendKvp(std::vector<uint8_t> &o, uint64_t &prev, uint64_t type,
               const uint8_t *raw, size_t raw_len, uint64_t num) {
    MoqAppendVarint(o, type - prev);
    prev = type;
    if ((type & 1) == 0) {
        MoqAppendVarint(o, num);
        return;
    }
    MoqAppendVarint(o, raw_len);
    MoqAppendBytes(o, raw, raw_len);
}

std::vector<uint8_t> EncodeProperties(
    const std::vector<std::pair<uint64_t, std::vector<uint8_t>>> &odd,
    const std::vector<std::pair<uint64_t, uint64_t>> &even) {
    struct Item {
        uint64_t type;
        bool is_odd;
        uint64_t num;
        std::vector<uint8_t> raw;
    };
    std::vector<Item> items;
    for (const auto &e : even) items.push_back({e.first, false, e.second, {}});
    for (const auto &e : odd) items.push_back({e.first, true, 0, e.second});
    std::sort(items.begin(), items.end(),
              [](const Item &a, const Item &b) { return a.type < b.type; });
    std::vector<uint8_t> body;
    uint64_t prev = 0;
    for (const auto &it : items) {
        if (it.is_odd) AppendKvp(body, prev, it.type, it.raw.data(), it.raw.size(), 0);
        else AppendKvp(body, prev, it.type, nullptr, 0, it.num);
    }
    std::vector<uint8_t> out;
    MoqAppendVarint(out, body.size());
    MoqAppendBytes(out, body.data(), body.size());
    return out;
}

std::vector<uint8_t> EncodeControl(uint64_t type, const std::vector<uint8_t> &body) {
    std::vector<uint8_t> out;
    MoqAppendVarint(out, type);
    MoqAppendU16(out, static_cast<uint16_t>(body.size()));
    MoqAppendBytes(out, body.data(), body.size());
    return out;
}

std::vector<uint8_t> EncodeSubgroup(uint64_t alias, uint64_t group) {
    std::vector<uint8_t> out;
    MoqAppendVarint(out, 0x31);
    MoqAppendVarint(out, alias);
    MoqAppendVarint(out, group);
    return out;
}

std::vector<uint8_t> EncodeLocObject(uint64_t delta, int64_t ts_ms,
                                     const uint8_t *payload, size_t payload_len,
                                     bool key, const uint8_t *cfg, size_t cfg_len,
                                     bool video_cfg, int64_t cts_ms) {
    std::vector<std::pair<uint64_t, uint64_t>> even = {
        {kLocTimescale, 1000},
        {kLocTimestamp, ts_ms < 0 ? 0 : static_cast<uint64_t>(ts_ms)},
    };
    /* 只有非零才带：绝大多数帧 cts=0，带上会平白增加每帧开销。
     * 订阅端读不到即按 0 处理，语义等价。 */
    if (cts_ms != 0) {
        even.push_back({kLocCompositionTime, static_cast<uint64_t>(cts_ms)});
    }
    std::vector<std::pair<uint64_t, std::vector<uint8_t>>> odd;
    if (key) odd.push_back({kLocFrameMark, {0x01}});
    if (cfg && cfg_len) {
        odd.push_back({video_cfg ? kLocVideoConfig : kLocAudioConfig,
                       std::vector<uint8_t>(cfg, cfg + cfg_len)});
    }
    std::vector<uint8_t> out;
    MoqAppendVarint(out, delta);
    const auto props = EncodeProperties(odd, even);
    MoqAppendBytes(out, props.data(), props.size());
    const size_t plen = payload && payload_len ? payload_len : 1;
    const uint8_t dummy = 0;
    MoqAppendVarint(out, plen);
    if (payload && payload_len) MoqAppendBytes(out, payload, payload_len);
    else out.push_back(dummy);
    return out;
}

} /* namespace */

std::unordered_map<std::string, std::unique_ptr<MoqPushSession>> MoqRfcHandler::sessions_;

bool MoqRfcHandler::IsMoqPath(const std::string &path) {
    return path == "/moq" || path == "moq";
}

void MoqRfcHandler::OnSession(WTServerSession &sess, const std::string &path) {
    LOG_INFO("[moq-rfc] session open path=%s id=%s",
             path.c_str(), sess.SessionId().c_str());
    auto old = sessions_.find(sess.SessionId());
    if (old != sessions_.end() && old->second) {
        old->second->Close();
        sessions_.erase(old);
    }
    sessions_[sess.SessionId()] = std::make_unique<MoqPushSession>();
}

void MoqRfcHandler::OnStreamData(WTServerSession &sess, WTServerStream &st,
                                 const uint8_t *data, size_t len,
                                 const std::string & /*path*/) {
    auto it = sessions_.find(sess.SessionId());
    if (it == sessions_.end()) {
        sessions_[sess.SessionId()] = std::make_unique<MoqPushSession>();
        it = sessions_.find(sess.SessionId());
    }
    it->second->OnData(sess, st, data, len);
}

void MoqRfcHandler::OnSessionClose(WTServerSession &sess, const std::string &path) {
    RemoveSubscriber(sess);
    const char *ev = "moq";
    if (sess.Method() == "push") ev = "moq_push";
    else if (sess.Method() == "pull") ev = "moq_pull";
    auto it = sessions_.find(sess.SessionId());
    if (it != sessions_.end()) {
        if (it->second->IsPublisher()) ev = "moq_push";
        else if (it->second->IsSubscriber()) ev = "moq_pull";
        if (it->second->IsPublisher() && it->second->HasMedia()) {
            MediaStreamManager::RemovePublisher(
                it->second->App() + "/" + it->second->Stream());
        }
        it->second->Close();
        sessions_.erase(it);
    }
    sess.StopMetaStats();
    MetaLog::Instance().ForgetSession(sess.SessionId());
    MetaLog::Instance().Event(ev, "closesession",
                              sess.App(), sess.Stream(), sess.ExtraParams());
    LOG_INFO("[moq-rfc] session close path=%s id=%s",
             path.c_str(), sess.SessionId().c_str());
}

void MoqRfcHandler::AddSubscriber(WTServerSession &sess, MoqPushSession &ms) {
    if (sess.App().empty() || sess.Stream().empty() || sess.SessionId().empty()) {
        LOG_WARN("[moq-rfc] SUBSCRIBE missing app/stream/id");
        return;
    }
    ms.AttachPlayer(sess.SessionId());
    LOG_INFO("[moq-rfc] subscribe add path=%s/%s id=%s",
             sess.App().c_str(), sess.Stream().c_str(), sess.SessionId().c_str());
}

void MoqRfcHandler::RemoveSubscriber(WTServerSession &sess) {
    auto it = sessions_.find(sess.SessionId());
    if (it == sessions_.end() || !it->second) return;
    it->second->DetachPlayer();
    LOG_INFO("[moq-rfc] subscribe del id=%s", sess.SessionId().c_str());
}

void MoqPushSession::Close() {
    DetachPlayer();
    if (video_dl_.st) video_dl_.st->SetOnWritable(nullptr);
    if (audio_dl_.st) audio_dl_.st->SetOnWritable(nullptr);
    ClearSendQueue(video_dl_);
    ClearSendQueue(audio_dl_);
    streams_.clear();
    control_ = nullptr;
    subscriber_sess_ = nullptr;
    video_dl_ = {};
    audio_dl_ = {};
    pull_ts_base_ = -1;
    last_pull_ts_ = -1;
}

void MoqPushSession::AttachPlayer(const std::string &writer_id) {
    if (player_added_) return;
    writer_id_ = writer_id;
    player_added_ = true;
    /* MoQ 的 GOP 在 OpenUniDownlink/SendGop 发，不走 MSM WriterGop。
     * Attach 时 downlink 还没绑，若让 AddPlayer 调 WriterGop，只会空跑并把
     * init_flag 提前置位；这里先标记已 init，跳过那次无效回放。 */
    init_flag_ = true;
    MediaStreamManager::AddPlayer(this);
}

void MoqPushSession::DetachPlayer() {
    if (!player_added_) return;
    MediaStreamManager::RemovePlayer(this);
    player_added_ = false;
}

int MoqPushSession::WritePacket(Media_Packet_Ptr pkt) {
    if (writer_closed_ || !pkt || !subscriber_) return 0;
    Downlink *dl = nullptr;
    uint64_t alias = 0;
    if (pkt->av_type_ == MEDIA_VIDEO_TYPE) {
        dl = &video_dl_;
        alias = kAliasVideo;
    } else if (pkt->av_type_ == MEDIA_AUDIO_TYPE) {
        dl = &audio_dl_;
        alias = kAliasAudio;
    } else {
        return 0;
    }
    /* 单向流开流失败（配额未到）时在这里重试 —— 否则这条 track 会永久哑掉：
     * 客户端 SUBSCRIBE_OK 收到了，却再也等不到 SUBGROUP。 */
    if (!dl->st && subscriber_sess_) OpenUniDownlink(*subscriber_sess_, alias);
    if (!dl->st || !dl->st->Valid() || !dl->header_sent) {
        static int drop_log = 0;
        if ((drop_log++ % 200) == 0) {
            LOG_WARN("[moq-rfc] live drop alias=%llu st=%d valid=%d hdr=%d",
                     (unsigned long long)alias,
                     dl->st ? 1 : 0,
                     (dl->st && dl->st->Valid()) ? 1 : 0,
                     dl->header_sent ? 1 : 0);
        }
        return 0;
    }
    if (dl->wait_key) {
        if (pkt->is_seq_hdr_ ||
            pkt->av_type_ != MEDIA_VIDEO_TYPE ||
            !pkt->is_key_frame_) {
            return 0;
        }
        dl->wait_key = false;
        LOG_INFO("[moq-rfc] subscribe start from key dts=%lld", (long long)pkt->dts_);
    }
    EmitLoc(*dl, alias, pkt);
    return 0;
}

std::string MoqPushSession::GetKey() {
    return app_ + "/" + stream_;
}

std::string MoqPushSession::GetWriterId() {
    return writer_id_;
}

void MoqPushSession::CloseWriter() {
    writer_closed_ = true;
}

bool MoqPushSession::IsInited() {
    return init_flag_;
}

void MoqPushSession::SetInitFlag(bool flag) {
    init_flag_ = flag;
}

MoqPushSession::StreamState &MoqPushSession::StateOf(WTServerStream &st) {
    return streams_[&st];
}

void MoqPushSession::OnData(WTServerSession &sess, WTServerStream &st,
                            const uint8_t *data, size_t len) {
    if (!data || len == 0) return;
    StreamState &ss = StateOf(st);
    ss.buf.insert(ss.buf.end(), data, data + len);
    Pump(sess, st, ss);
}

void MoqPushSession::Pump(WTServerSession &sess, WTServerStream &st,
                          StreamState &ss) {
    for (;;) {
        if (ss.kind == StreamState::UNKNOWN) {
            if (ss.buf.empty()) return;
            size_t off = 0;
            uint64_t type = 0;
            if (!MoqReadVarint(ss.buf.data(), ss.buf.size(), off, type)) return;
            LogFirstBytes(ss.buf.data(), ss.buf.size());
            const char *cls = ClassifyFirstType(type, ss.buf.data(), ss.buf.size());
            /* 控制流（单向）：只有 SETUP / GOAWAY。
             * 请求流（双向）：PUBLISH / SUBSCRIBE 各占一条，响应走本流反向。
             * 见 draft-ietf-moq-transport §3.3。 */
            if (type == kMoqtSetup || type == kMoqtGoaway) {
                ss.kind = StreamState::CONTROL;
                control_ = &st;
                LOG_INFO("[moq-rfc] stream CONTROL first type=0x%llx (%s)",
                         (unsigned long long)type, cls);
            } else if (type == kMoqtPublish || type == kMoqtSubscribe) {
                ss.kind = StreamState::REQUEST;
                LOG_INFO("[moq-rfc] stream REQUEST first type=0x%llx (%s)",
                         (unsigned long long)type, cls);
            } else if ((type & 0x10) != 0) {
                ss.kind = StreamState::DATA;
                LOG_INFO("[moq-rfc] stream DATA first type=0x%llx (%s)",
                         (unsigned long long)type, cls);
            } else {
                LOG_WARN("[moq-rfc] unknown stream first type=%llu (%s) — "
                         "expect SETUP/PUBLISH/SUBSCRIBE",
                         (unsigned long long)type, cls);
                ss.buf.clear();
                return;
            }
        }
        bool ok = false;
        if (ss.kind == StreamState::CONTROL || ss.kind == StreamState::REQUEST) {
            /* 两类流都用同一套「varint type + u16 len + body」解析 */
            ok = ParseControl(sess, st, ss);
        } else if (!ss.header_done) {
            ok = ParseSubgroupHeader(ss);
        } else {
            ok = ParseObject(ss);
        }
        if (!ok) return;
    }
}

bool MoqPushSession::ParseControl(WTServerSession &sess, WTServerStream &st,
                                  StreamState &ss) {
    size_t off = 0;
    uint64_t type = 0;
    uint16_t length = 0;
    if (!MoqReadVarint(ss.buf.data(), ss.buf.size(), off, type)) return false;
    if (!MoqReadU16(ss.buf.data(), ss.buf.size(), off, length)) return false;
    if (off + length > ss.buf.size()) return false;
    const uint8_t *body = ss.buf.data() + off;
    if (type == kMoqtPublish) {
        LOG_INFO("[moq-rfc] RX PUBLISH body=%uB", (unsigned)length);
        if (ParsePublish(body, length)) {
            const bool first = !publisher_;
            publisher_ = true;
            sess.SetPublish(app_, stream_);
            if (first) {
                MetaLog::Instance().Event("moq_push", "opensession",
                                          app_, stream_, sess.ExtraParams());
                sess.StartMetaStats("moq_push_stats", "stream_push");
            }
            LOG_INFO("[moq-rfc] PUBLISH ok app=%s stream=%s",
                     app_.c_str(), stream_.c_str());
        } else {
            LOG_WARN("[moq-rfc] PUBLISH parse failed body=%uB", (unsigned)length);
        }
    } else if (type == kMoqtSubscribe) {
        uint64_t request_id = 0;
        uint64_t alias = 0;
        std::string name;
        LOG_INFO("[moq-rfc] RX SUBSCRIBE body=%uB", (unsigned)length);
        if (ParseSubscribe(body, length, request_id, alias, name)) {
            const bool first = !subscriber_;
            subscriber_ = true;
            sess.SetSubscribe(app_, stream_);
            MoqRfcHandler::AddSubscriber(sess, *this);
            if (first) {
                MetaLog::Instance().Event("moq_pull", "opensession",
                                          app_, stream_, sess.ExtraParams());
                sess.StartMetaStats("moq_pull_stats", "stream_pull");
            }
            /* 应答走请求流自身的反向（draft §3.3.2） */
            SendSubscribeOk(st, request_id, alias);
            LOG_INFO("[moq-rfc] SUBSCRIBE ok req=%llu name=%s alias=%llu ns=%s/%s",
                     (unsigned long long)request_id, name.c_str(),
                     (unsigned long long)alias, app_.c_str(), stream_.c_str());
            /* 对象单向流由本端发起；这里只是首次尝试，开不出来（配额未到）
             * 由 WritePacket 兜底重试，不当作失败。 */
            subscriber_sess_ = &sess;
            if (!OpenUniDownlink(sess, alias)) {
                LOG_WARN("[moq-rfc] open uni downlink alias=%llu deferred",
                         (unsigned long long)alias);
            }
        } else {
            LOG_WARN("[moq-rfc] SUBSCRIBE parse failed body=%uB", (unsigned)length);
        }
    } else if (type == kMoqtSetup) {
        LOG_INFO("[moq-rfc] RX SETUP body=%uB", (unsigned)length);
    } else {
        LOG_WARN("[moq-rfc] RX control type=0x%llx body=%uB",
                 (unsigned long long)type, (unsigned)length);
    }
    ss.buf.erase(ss.buf.begin(), ss.buf.begin() + static_cast<long>(off + length));
    return true;
}

bool MoqPushSession::ParsePublish(const uint8_t *body, size_t len) {
    size_t off = 0;
    uint64_t request_id = 0;
    uint64_t ns_n = 0;
    if (!MoqReadVarint(body, len, off, request_id)) return false;
    if (!MoqReadVarint(body, len, off, ns_n)) return false;
    std::string app;
    std::string stream;
    for (uint64_t i = 0; i < ns_n; i++) {
        uint64_t flen = 0;
        const uint8_t *f = nullptr;
        if (!MoqReadVarint(body, len, off, flen)) return false;
        if (!ReadBytes(body, len, off, static_cast<size_t>(flen), &f)) return false;
        std::string field(reinterpret_cast<const char *>(f), static_cast<size_t>(flen));
        if (i == 0) app = field;
        else if (i == 1) stream = field;
    }
    uint64_t nlen = 0;
    const uint8_t *namep = nullptr;
    uint64_t alias = 0;
    uint64_t nparam = 0;
    if (!MoqReadVarint(body, len, off, nlen)) return false;
    if (!ReadBytes(body, len, off, static_cast<size_t>(nlen), &namep)) return false;
    if (!MoqReadVarint(body, len, off, alias)) return false;
    if (!MoqReadVarint(body, len, off, nparam)) return false;
    if (nparam != 0) {
        LOG_WARN("[moq-rfc] PUBLISH params=%llu not skipped fully",
                 (unsigned long long)nparam);
    }
    std::string name(reinterpret_cast<const char *>(namep), static_cast<size_t>(nlen));
    if (!app.empty()) app_ = app;
    if (!stream.empty()) stream_ = stream;
    LOG_INFO("[moq-rfc] PUBLISH req=%llu name=%s alias=%llu ns=%s/%s",
             (unsigned long long)request_id, name.c_str(),
             (unsigned long long)alias, app_.c_str(), stream_.c_str());
    return !app_.empty() && !stream_.empty();
}

bool MoqPushSession::ParseSubscribe(const uint8_t *body, size_t len,
                                    uint64_t &request_id, uint64_t &alias,
                                    std::string &name) {
    size_t off = 0;
    uint64_t ns_n = 0;
    if (!MoqReadVarint(body, len, off, request_id)) return false;
    if (!MoqReadVarint(body, len, off, ns_n)) return false;
    std::string app;
    std::string stream;
    for (uint64_t i = 0; i < ns_n; i++) {
        uint64_t flen = 0;
        const uint8_t *f = nullptr;
        if (!MoqReadVarint(body, len, off, flen)) return false;
        if (!ReadBytes(body, len, off, static_cast<size_t>(flen), &f)) return false;
        std::string field(reinterpret_cast<const char *>(f), static_cast<size_t>(flen));
        if (i == 0) app = field;
        else if (i == 1) stream = field;
    }
    uint64_t nlen = 0;
    const uint8_t *namep = nullptr;
    if (!MoqReadVarint(body, len, off, nlen)) return false;
    if (!ReadBytes(body, len, off, static_cast<size_t>(nlen), &namep)) return false;
    name.assign(reinterpret_cast<const char *>(namep), static_cast<size_t>(nlen));
    alias = 0;
    if (off < len) MoqReadVarint(body, len, off, alias);
    if (alias == 0) {
        if (name == "video") alias = kAliasVideo;
        else if (name == "audio") alias = kAliasAudio;
    }
    if (!app.empty()) app_ = app;
    if (!stream.empty()) stream_ = stream;
    return !app_.empty() && !stream_.empty();
}

/* 应答写在【请求流自身的反向】。
 * draft §3.3.2：对端收到请求后必须把对应的响应消息发回该请求流，
 * 而不是另开流 —— 所以这里直接写传进来的 st。 */
void MoqPushSession::SendSubscribeOk(WTServerStream &st, uint64_t request_id,
                                     uint64_t alias) {
    std::vector<uint8_t> body;
    MoqAppendVarint(body, request_id);
    MoqAppendVarint(body, alias);
    MoqAppendVarint(body, 0); /* expires */
    const auto msg = EncodeControl(kMoqtSubscribeOk, body);
    st.Write(msg.data(), msg.size());
    LOG_INFO("[moq-rfc] TX SUBSCRIBE_OK req=%llu alias=%llu %zuB",
             (unsigned long long)request_id, (unsigned long long)alias, msg.size());
}

/* 订阅方向的数据单向流 —— 由【服务端】发起（对象走单向流）。
 * 客户端不再开双向 bind 流；这条流属于哪个 track 由 SUBGROUP 头里的
 * Track Alias 表达，不再靠"对端开流并写 alias"来绑定。
 *
 * 返回 false 表示这条 track 的流暂时没开出来 —— 通常是客户端尚未给出
 * MAX_STREAMS_UNI 配额（见 quic/quic_stream.c 的流控检查）。这是正常
 * 时序，由 WritePacket 在下一帧重试，不是致命错误。 */
bool MoqPushSession::OpenUniDownlink(WTServerSession &sess, uint64_t alias) {
    Downlink *dl = nullptr;
    if (alias == kAliasVideo) dl = &video_dl_;
    else if (alias == kAliasAudio) dl = &audio_dl_;
    else {
        LOG_INFO("[moq-rfc] subscribe alias=%llu ignored",
                 (unsigned long long)alias);
        return false;
    }
    if (dl->st && dl->st->Valid()) return true;   /* 已就绪，别重开 */

    WTServerStream *st = sess.OpenUniStream();
    if (!st) return false;                        /* 配额未到，下帧再来 */

    dl->st = st;
    dl->header_sent = false;
    dl->obj = 0;
    dl->wait_key = false;
    dl->congested = false;
    ClearSendQueue(*dl);
    st->SetOnWritable([this, alias](WTServerStream &) {
        Downlink *d = nullptr;
        if (alias == kAliasVideo) d = &video_dl_;
        else if (alias == kAliasAudio) d = &audio_dl_;
        if (d) DrainSendQueue(*d);
    });
    const auto hdr = EncodeSubgroup(alias, 0);
    const int wr = st->Write(hdr.data(), hdr.size());
    if (wr == 1) {
        auto buf = std::make_shared<DataBuffer>(hdr.size() + 64);
        buf->AppendData(reinterpret_cast<const char *>(hdr.data()), hdr.size());
        EnqueueSend(*dl, buf);
        dl->congested = true;
        LOG_WARN("[moq-rfc] downlink alias=%llu subgroup queued (congested)",
                 (unsigned long long)alias);
    } else if (wr < 0) {
        LOG_WARN("[moq-rfc] downlink alias=%llu subgroup write fail",
                 (unsigned long long)alias);
        dl->st = nullptr;                         /* 留待下一帧重开 */
        return false;
    }
    dl->header_sent = true;
    LOG_INFO("[moq-rfc] downlink alias=%llu uni stream ready",
             (unsigned long long)alias);
    SendGop(*dl, alias);   /* 订阅即回放缓存 GOP，行为与改动前一致 */
    return true;
}

void MoqPushSession::SendGop(Downlink &dl, uint64_t alias) {
    GopCache *gop = MediaStreamManager::GetGop(app_ + "/" + stream_);
    if (!gop) {
        LOG_INFO("[moq-rfc] subscribe gop empty alias=%llu, wait live",
                 (unsigned long long)alias);
        return;
    }
    EnsurePullTsBase();
    if (alias == kAliasVideo && gop->VideoHdr()) EmitLoc(dl, alias, gop->VideoHdr());
    if (alias == kAliasAudio && gop->AudioHdr()) EmitLoc(dl, alias, gop->AudioHdr());

    int64_t last_video_dts = -1;
    int64_t last_audio_dts = -1;
    LOG_INFO("[moq-rfc] subscribe gop packet count=%llu pull_ts_base=%lld",
             (unsigned long long)gop->Packets().size(), (long long)pull_ts_base_);
    for (const auto &pkt : gop->Packets()) {
        if (!pkt) continue;
        if (pkt->av_type_ == MEDIA_VIDEO_TYPE) last_video_dts = pkt->dts_;
        if (pkt->av_type_ == MEDIA_AUDIO_TYPE) last_audio_dts = pkt->dts_;
    }
    if (alias == kAliasVideo && last_video_dts >= 0 && last_audio_dts >= 0 &&
        last_audio_dts - last_video_dts > 400) {
        dl.wait_key = true;
        LOG_INFO("[moq-rfc] subscribe skip stale gop video_end=%lld audio_end=%lld",
                 (long long)last_video_dts, (long long)last_audio_dts);
        return;
    }

    size_t sent = 0;
    for (const auto &pkt : gop->Packets()) {
        if (!pkt) continue;
        if (alias == kAliasVideo && pkt->av_type_ != MEDIA_VIDEO_TYPE) continue;
        if (alias == kAliasAudio && pkt->av_type_ != MEDIA_AUDIO_TYPE) continue;
        if (alias == kAliasAudio && last_video_dts >= 0 &&
            pkt->dts_ > last_video_dts + 40) {
            continue;
        }
        EmitLoc(dl, alias, pkt);
        sent++;
    }
    LOG_INFO("[moq-rfc] subscribe gop alias=%llu packets=%zu",
             (unsigned long long)alias, sent);
}

void MoqPushSession::EnsurePullTsBase() {
    if (pull_ts_base_ >= 0) return;
    GopCache *gop = MediaStreamManager::GetGop(app_ + "/" + stream_);
    if (!gop) return;
    int64_t last_v = -1;
    for (const auto &p : gop->Packets()) {
        if (p && p->av_type_ == MEDIA_VIDEO_TYPE && !p->is_seq_hdr_ && p->dts_ > 0) {
            last_v = p->dts_;
        }
    }
    /* 只用 GOP 末尾附近的包做原点，忽略残留的旧低 dts，否则 PullTs 会到几十万 */
    const int64_t edge = last_v;
    const int64_t window_ms = 30000;
    int64_t origin = -1;
    for (const auto &p : gop->Packets()) {
        if (!p || p->is_seq_hdr_ || p->dts_ <= 0) continue;
        if (last_v >= 0 && p->av_type_ == MEDIA_AUDIO_TYPE &&
            p->dts_ > last_v + 40) {
            continue;
        }
        if (edge >= 0 && p->dts_ + window_ms < edge) continue;
        if (origin < 0 || p->dts_ < origin) origin = p->dts_;
    }
    if (origin < 0 && edge >= 0) origin = edge;
    if (origin < 0) return;
    pull_ts_base_ = origin;
    LOG_INFO("[moq-rfc] pull ts_base=%lld (gop edge window last_v=%lld)",
             (long long)pull_ts_base_, (long long)last_v);
}

int64_t MoqPushSession::PullTs(Media_Packet_Ptr pkt) {
    if (!pkt) return 0;
    const int64_t dts = pkt->dts_ < 0 ? 0 : pkt->dts_;
    if (pkt->is_seq_hdr_ && pull_ts_base_ < 0) return 0;
    if (pull_ts_base_ < 0) {
        if (dts <= 0) return 0;
        pull_ts_base_ = dts;
        LOG_INFO("[moq-rfc] pull ts_base=%lld", (long long)pull_ts_base_);
    }
    int64_t ts = dts - pull_ts_base_;
    if (ts < 0) ts = 0;
    if (!pkt->is_seq_hdr_) {
        /* 首包媒体已离原点很远（无 GOP / base 过旧）：直接从 0 起播 */
        if (last_pull_ts_ < 0 && ts > 5000) {
            LOG_INFO("[moq-rfc] pull ts rebase first dts=%lld old_base=%lld -> 0",
                     (long long)dts, (long long)pull_ts_base_);
            pull_ts_base_ = dts;
            ts = 0;
        } else if (last_pull_ts_ >= 0 && ts > last_pull_ts_ + 5000) {
            /* GOP/live 或音视频时钟错位：相对 ts 一次跳几分钟，重锚到上一包附近 */
            const int64_t old_base = pull_ts_base_;
            const int64_t old_ts = ts;
            pull_ts_base_ = dts - (last_pull_ts_ + 40);
            ts = dts - pull_ts_base_;
            if (ts < 0) ts = 0;
            LOG_INFO("[moq-rfc] pull ts rebase old_base=%lld new_base=%lld dts=%lld ts=%lld->%lld",
                     (long long)old_base, (long long)pull_ts_base_, (long long)dts,
                     (long long)old_ts, (long long)ts);
            /* 队列里是已编码旧 ts，与新时间轴混发会导致前端 rewind/大间隙 */
            ClearSendQueue(video_dl_);
            ClearSendQueue(audio_dl_);
        }
        last_pull_ts_ = ts;
    }
    return ts;
}

void MoqPushSession::ClearSendQueue(Downlink &dl) {
    while (!dl.send_q.empty()) dl.send_q.pop();
    dl.send_q_bytes = 0;
    dl.congested = false;
}

void MoqPushSession::TrimSendQueue(Downlink &dl) {
    /* 直播优先保实时：队列过大时丢最旧的已编码对象 */
    static const size_t kMaxQBytes = 2u * 1024u * 1024u;
    static const size_t kMaxQItems = 512;
    while ((!dl.send_q.empty()) &&
           (dl.send_q_bytes > kMaxQBytes || dl.send_q.size() > kMaxQItems)) {
        auto &front = dl.send_q.front();
        if (front) {
            const size_t n = static_cast<size_t>(front->DataLen());
            if (dl.send_q_bytes >= n) dl.send_q_bytes -= n;
            else dl.send_q_bytes = 0;
        }
        dl.send_q.pop();
    }
}

void MoqPushSession::EnqueueSend(Downlink &dl, std::shared_ptr<DataBuffer> buf) {
    if (!buf || buf->DataLen() <= 0) return;
    dl.send_q_bytes += static_cast<size_t>(buf->DataLen());
    dl.send_q.push(std::move(buf));
    TrimSendQueue(dl);
}

void MoqPushSession::DrainSendQueue(Downlink &dl) {
    if (!dl.st || !dl.st->Valid()) {
        ClearSendQueue(dl);
        return;
    }
    while (!dl.send_q.empty()) {
        auto &front = dl.send_q.front();
        if (!front || front->DataLen() <= 0) {
            dl.send_q.pop();
            continue;
        }
        const int r = dl.st->Write(
            reinterpret_cast<const uint8_t *>(front->Data()),
            static_cast<size_t>(front->DataLen()));
        if (r == 1) {
            dl.congested = true;
            int64_t now_ms = now_millisec();
            if ((now_ms/1000) != dl.last_congested_dbg_ts_s) {
                dl.last_congested_dbg_ts_s = now_ms/1000;
                LOG_WARN("[moq-rfc] drain write congested, drop queued=%zu bytes=%zu",
                         dl.send_q.size(), dl.send_q_bytes);
            }
            return;
        }
        if (r < 0) {
            LOG_WARN("[moq-rfc] drain write fail, drop queued=%zu bytes=%zu",
                     dl.send_q.size(), dl.send_q_bytes);
            ClearSendQueue(dl);
            return;
        }
        dl.send_q_bytes -= static_cast<size_t>(front->DataLen());
        dl.send_q.pop();
    }
    dl.congested = false;
    /* 队列已排空，是正常状态 —— 每秒打一条 WARN 只会淹没上面的
     * "congested" / "fail" 两条真正需要关注的告警（它们外观一样）。
     * 降到 DEBUG：排查积压时按需打开即可。 */
    LOG_DEBUG("[moq-rfc] drain write success, drop queued=%zu bytes=%zu",
              dl.send_q.size(), dl.send_q_bytes);
}

void MoqPushSession::EmitLoc(Downlink &dl, uint64_t alias, Media_Packet_Ptr pkt) {
    if (!dl.st || !pkt || !pkt->buffer_ptr_) return;
    const auto *body = reinterpret_cast<const uint8_t *>(pkt->buffer_ptr_->Data());
    const size_t body_len = pkt->buffer_ptr_->DataLen();
    if (!body || body_len == 0) return;

    const uint8_t *cfg = nullptr;
    size_t cfg_len = 0;
    const uint8_t *payload = nullptr;
    size_t payload_len = 0;
    bool video_cfg = alias == kAliasVideo;
    bool key = pkt->is_key_frame_;

    /* 视频 tag body 前 5 字节是 [frame|codec][AVCPacketType][CTS×3]。
     * CTS 必须单独取出来：它是 24 位有符号、可能为负，且要用 LOC 的
     * CompositionTime 属性重新下发，否则订阅端拿到的 pts 恒等于 dts，
     * 有 B 帧的流在 MSE 上会前后跳。 */
    int64_t cts_ms = 0;
    if (alias == kAliasVideo) {
        if (body_len < 5) return;
        {
            int32_t c = (static_cast<int32_t>(body[2]) << 16) |
                        (static_cast<int32_t>(body[3]) << 8) |
                        static_cast<int32_t>(body[4]);
            if (c & 0x800000) c -= 0x1000000;   /* 24 位补码 → 有符号 */
            cts_ms = pkt->is_seq_hdr_ ? 0 : c;  /* config 帧无显示时间 */
        }
        payload = body + 5;
        payload_len = body_len - 5;
        if (pkt->is_seq_hdr_) {
            cfg = payload;
            cfg_len = payload_len;
        }
    } else {
        if (body_len < 2) return;
        payload = body + 2;
        payload_len = body_len - 2;
        if (pkt->is_seq_hdr_) {
            cfg = payload;
            cfg_len = payload_len;
        }
    }
    if (!payload || payload_len == 0) return;

    const int64_t ts = PullTs(pkt);

    const auto obj = EncodeLocObject(dl.obj == 0 ? 0 : 0, ts, payload, payload_len,
                                     key && !pkt->is_seq_hdr_, cfg, cfg_len, video_cfg,
                                     cts_ms);
    auto buf = std::make_shared<DataBuffer>(obj.size() + 64);
    buf->AppendData(reinterpret_cast<const char *>(obj.data()), obj.size());

    DrainSendQueue(dl);
    if (!dl.congested && dl.send_q.empty()) {
        const int r = dl.st->Write(
            reinterpret_cast<const uint8_t *>(buf->Data()),
            static_cast<size_t>(buf->DataLen()));
        if (r == 0) {
            /* accepted */
        } else if (r == 1) {
            dl.congested = true;
            EnqueueSend(dl, buf);
        } else {
            LOG_WARN("[moq-rfc] TX LOC write fail alias=%llu",
                     (unsigned long long)alias);
            return;
        }
    } else {
        EnqueueSend(dl, buf);
    }

    dl.obj += 1;
    int64_t now_ms = now_millisec();
    if (dl.last_send_dbg_ts_s != now_ms / 1000) {
        dl.last_send_dbg_ts_s = now_ms / 1000;
        LOG_INFO("[moq-rfc] TX LOC alias=%llu n=%llu ts=%lld dts=%lld size=%zu key=%d q=%zu/%zu congested=%d",
                 (unsigned long long)alias, (unsigned long long)dl.obj,
                 (long long)ts, (long long)(pkt->dts_ < 0 ? 0 : pkt->dts_),
                 payload_len, key ? 1 : 0,
                 dl.send_q.size(), dl.send_q_bytes, dl.congested ? 1 : 0);
    }
    LOG_DEBUG("[moq-rfc] TX LOC alias=%llu ts=%lld size=%zu seq=%d key=%d",
              (unsigned long long)alias, (long long)ts, payload_len,
              (int)pkt->is_seq_hdr_, key ? 1 : 0);
}

bool MoqPushSession::ParseSubgroupHeader(StreamState &ss) {
    size_t off = 0;
    uint64_t flags = 0;
    uint64_t alias = 0;
    uint64_t group = 0;
    if (!MoqReadVarint(ss.buf.data(), ss.buf.size(), off, flags)) return false;
    if (!MoqReadVarint(ss.buf.data(), ss.buf.size(), off, alias)) return false;
    if (!MoqReadVarint(ss.buf.data(), ss.buf.size(), off, group)) return false;
    const uint64_t id_mode = (flags >> 1) & 0x03;
    if (id_mode == 0x02) {
        uint64_t sg = 0;
        if (!MoqReadVarint(ss.buf.data(), ss.buf.size(), off, sg)) return false;
    }
    if ((flags & 0x20) == 0) {
        if (off >= ss.buf.size()) return false;
        off += 1;
    }
    ss.has_props = (flags & 0x01) != 0;
    ss.alias = alias;
    ss.header_done = true;
    ss.buf.erase(ss.buf.begin(), ss.buf.begin() + static_cast<long>(off));
    LOG_INFO("[moq-rfc] subgroup alias=%llu group=%llu props=%d",
             (unsigned long long)alias, (unsigned long long)group,
             ss.has_props ? 1 : 0);
    return true;
}

bool MoqPushSession::ParseObject(StreamState &ss) {
    size_t off = 0;
    uint64_t delta = 0;
    if (!MoqReadVarint(ss.buf.data(), ss.buf.size(), off, delta)) return false;

    int64_t ts_ms = 0;
    /* pts - dts。缺省 0（等价于 pts == dts，即无 B 帧的流）。
     * 客户端只在非零时携带该属性，所以读不到是正常情况。 */
    int64_t cts_ms = 0;
    bool key = false;
    const uint8_t *cfg = nullptr;
    size_t cfg_len = 0;
    uint64_t cfg_type = 0;

    if (ss.has_props) {
        uint64_t plen = 0;
        if (!MoqReadVarint(ss.buf.data(), ss.buf.size(), off, plen)) return false;
        if (off + plen > ss.buf.size()) return false;
        const size_t pend = off + static_cast<size_t>(plen);
        uint64_t prev = 0;
        while (off < pend) {
            uint64_t dlt = 0;
            if (!MoqReadVarint(ss.buf.data(), ss.buf.size(), off, dlt)) return false;
            const uint64_t type = prev + dlt;
            prev = type;
            if ((type & 1) == 0) {
                uint64_t val = 0;
                if (!MoqReadVarint(ss.buf.data(), ss.buf.size(), off, val)) return false;
                if (type == kLocTimestamp) ts_ms = static_cast<int64_t>(val);
                if (type == kLocCompositionTime) cts_ms = static_cast<int64_t>(val);
                (void)kLocTimescale;
            } else {
                uint64_t vlen = 0;
                const uint8_t *v = nullptr;
                if (!MoqReadVarint(ss.buf.data(), ss.buf.size(), off, vlen)) return false;
                if (!ReadBytes(ss.buf.data(), ss.buf.size(), off, static_cast<size_t>(vlen), &v)) {
                    return false;
                }
                if (type == kLocFrameMark && vlen > 0 && (v[0] & 0x01)) key = true;
                if (type == kLocVideoConfig || type == kLocAudioConfig) {
                    cfg = v;
                    cfg_len = static_cast<size_t>(vlen);
                    cfg_type = type;
                }
            }
        }
        if (off > pend) return false;
        off = pend;
    }

    uint64_t payload_len = 0;
    if (!MoqReadVarint(ss.buf.data(), ss.buf.size(), off, payload_len)) return false;
    if (payload_len == 0) {
        uint64_t status = 0;
        if (!MoqReadVarint(ss.buf.data(), ss.buf.size(), off, status)) return false;
        ss.buf.erase(ss.buf.begin(), ss.buf.begin() + static_cast<long>(off));
        return true;
    }
    const uint8_t *payload = nullptr;
    if (!ReadBytes(ss.buf.data(), ss.buf.size(), off,
                   static_cast<size_t>(payload_len), &payload)) {
        return false;
    }

    const uint64_t obj = ss.last_obj_set ? ss.last_obj + delta + 1 : delta;
    ss.last_obj = obj;
    ss.last_obj_set = true;

    std::vector<uint8_t> cfg_copy;
    if (cfg && cfg_len) cfg_copy.assign(cfg, cfg + cfg_len);
    std::vector<uint8_t> pay(payload, payload + static_cast<size_t>(payload_len));
    ss.buf.erase(ss.buf.begin(), ss.buf.begin() + static_cast<long>(off));

    OnLocObject(ss.alias, ts_ms, cts_ms, key,
                cfg_copy.empty() ? nullptr : cfg_copy.data(), cfg_copy.size(),
                pay.data(), pay.size());
    (void)cfg_type;
    return true;
}

void MoqPushSession::OnLocObject(uint64_t alias, int64_t ts_ms, int64_t cts_ms,
                                 bool key, const uint8_t *cfg, size_t cfg_len,
                                 const uint8_t *payload, size_t payload_len) {
    if (!HasMedia() || !payload || payload_len == 0) return;
    if (alias == kAliasVideo) {
        if (cfg && cfg_len && !avc_seq_sent_) {
            /* config 帧没有显示时间，CTS 恒 0 */
            EmitFlvVideo(ts_ms, true, true, cfg, cfg_len, 0);
            avc_seq_sent_ = true;
        }
        EmitFlvVideo(ts_ms, false, key, payload, payload_len, cts_ms);
        return;
    }
    if (alias == kAliasAudio) {
        if (cfg && cfg_len && !aac_seq_sent_) {
            EmitFlvAudio(ts_ms, true, cfg, cfg_len);
            aac_seq_sent_ = true;
        }
        EmitFlvAudio(ts_ms, false, payload, payload_len);
    }
}

void MoqPushSession::EmitFlvVideo(int64_t dts, bool seq, bool key,
                                  const uint8_t *data, size_t len,
                                  int64_t cts_ms) {
    const size_t body_len = 5 + len;
    auto pkt = std::make_shared<Media_Packet>(body_len);
    pkt->av_type_ = MEDIA_VIDEO_TYPE;
    pkt->codec_type_ = MEDIA_CODEC_H264;
    pkt->fmt_type_ = MEDIA_FORMAT_FLV;
    pkt->dts_ = dts;
    pkt->is_seq_hdr_ = seq;
    pkt->is_key_frame_ = !seq && key;
    pkt->app_ = app_;
    pkt->streamname_ = stream_;
    pkt->key_ = app_ + "/" + stream_;
    /* CompositionTime 是 24 位有符号，写 DTS 与 PTS 的差。
     * 写 0（旧行为）会让播放端认为 pts == dts：有 B 帧的流解码顺序
     * 与显示顺序不一致，MSE/flv.js 严格按容器时间戳渲染 → 画面前后跳。
     * 负数在 FLV 里是合法值（B 帧需要），按 24 位补码写入。 */
    const int64_t cts = seq ? 0 : cts_ms;
    const uint32_t cts_u = static_cast<uint32_t>(cts) & 0xffffffu;
    uint8_t hdr[5] = {
        static_cast<uint8_t>((seq || key) ? (FLV_VIDEO_KEY_FLAG | FLV_VIDEO_H264_CODEC)
                                          : (FLV_VIDEO_INTER_FLAG | FLV_VIDEO_H264_CODEC)),
        static_cast<uint8_t>(seq ? FLV_VIDEO_AVC_SEQHDR : FLV_VIDEO_AVC_NALU),
        static_cast<uint8_t>((cts_u >> 16) & 0xff),
        static_cast<uint8_t>((cts_u >> 8) & 0xff),
        static_cast<uint8_t>(cts_u & 0xff),
    };
    pkt->buffer_ptr_->AppendData(reinterpret_cast<const char *>(hdr), 5);
    pkt->buffer_ptr_->AppendData(reinterpret_cast<const char *>(data), len);
    /* pts = dts + cts。NormalizeDts 之后会平移 dts 但保留两者之差，
     * 所以这个偏移能一路活到写 FLV tag 的时候。 */
    pkt->pts_ = dts + cts;
    MediaStreamManager::WriterMediaPacket(pkt);
    if (cts != 0) {
        LOG_DEBUG("[moq-rfc] flv video %s dts=%lld cts=%lld pts=%lld size=%zu key=%d",
                 seq ? "seq" : "nalu", (long long)dts, (long long)cts,
                 (long long)(dts + cts), len, key ? 1 : 0);
    }
}

void MoqPushSession::EmitFlvAudio(int64_t dts, bool seq,
                                  const uint8_t *data, size_t len) {
    const size_t body_len = 2 + len;
    auto pkt = std::make_shared<Media_Packet>(body_len);
    pkt->av_type_ = MEDIA_AUDIO_TYPE;
    pkt->codec_type_ = MEDIA_CODEC_AAC;
    pkt->fmt_type_ = MEDIA_FORMAT_FLV;
    pkt->dts_ = dts;
    pkt->pts_ = dts;
    pkt->is_seq_hdr_ = seq;
    pkt->is_key_frame_ = seq;
    pkt->app_ = app_;
    pkt->streamname_ = stream_;
    pkt->key_ = app_ + "/" + stream_;
    const uint8_t hdr[2] = {
        static_cast<uint8_t>(FLV_AUDIO_AAC_CODEC | 0x0f),
        static_cast<uint8_t>(seq ? 0x00 : 0x01),
    };
    pkt->buffer_ptr_->AppendData(reinterpret_cast<const char *>(hdr), 2);
    pkt->buffer_ptr_->AppendData(reinterpret_cast<const char *>(data), len);
    MediaStreamManager::WriterMediaPacket(pkt);
    LOG_DEBUG("[moq-rfc] flv audio %s dts=%lld size=%zu",
             seq ? "seq" : "raw", (long long)dts, len);
}

} /* namespace cpp_streamer */
