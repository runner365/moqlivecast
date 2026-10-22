#include "moq/moq_lite_session.hpp"
#include "moq/moq_lite_wire.hpp"

#include "av.hpp"
#include "flv_pub.hpp"
#include "logger.h"
#include "media_packet.hpp"
#include "media_stream_manager.hpp"
#include "quic_listener.h"
#include "utils/json.hpp"
#include "utils/timeex.hpp"

#include <algorithm>
#include <cstdio>

namespace cpp_streamer {

namespace {

/* -06 §4.4.1 会话错误码 */
constexpr uint64_t kSessionInternalError     = 0x1;
constexpr uint64_t kSessionUnauthorized      = 0x2;
constexpr uint64_t kSessionProtocolViolation = 0x3;

/* -06 §4.4.2 流错误码。草案没有「不支持」这一类，未知流类型只能挑一个通用的。 */
constexpr uint64_t kStreamInternalError = 0x0;
/* CANCELLED —— 参考实现在「GROUP 的 Subscribe ID 没订阅过」时用的就是这个
 * （rs/lite/subscriber.rs:765-783 把找不到的 id 当取消处理）。 */
constexpr uint64_t kStreamCancelled = 0x1;

/* -06 §7.2 流类型。§5 把流分成两个命名空间：
 *   双向（§5.1）0x1 Announce / 0x2 Subscribe / 0x3 Fetch / 0x4 Probe /
 *                0x5 Goaway  / 0x6 Track
 *   单向（§6.3）0x0 Group    / 0x1 Setup
 * 注意 0x1 在两个命名空间里含义不同 —— 靠 QUIC 流 ID 的 0x2 位区分（RFC 9000 §2.1）。 */
constexpr uint64_t kBidiAnnounce  = 0x1;
constexpr uint64_t kBidiSubscribe = 0x2;
constexpr uint64_t kBidiFetch     = 0x3;
constexpr uint64_t kBidiProbe     = 0x4;
constexpr uint64_t kBidiGoaway    = 0x5;
constexpr uint64_t kBidiTrack     = 0x6;

constexpr uint64_t kUniGroup = 0x0;
constexpr uint64_t kUniSetup = 0x1;

constexpr uint64_t kStreamIdUniBit = 0x2;

/* ── SUBSCRIBE 的 Subscriber Max Age ──
 *
 * ⚠️ 这个字段**不是**「不设上限 = 0」，而是**发布端为我们保留多久的历史**的
 * 容忍度（staleness tolerance）。0 = **零容忍**：发布端把读游标钉在最新组上，
 * 任何「头已经不在缓存里」的组整组跳过 —— 因为 lite 里 **一个组 = 一条 uni
 * 流 = 解码单元**（参考实现 `position_group` 的文档原话："a group is the unit
 * of decodability"），头没了就只剩一条只能 reset 的流，不如不发。
 * 参考实现里那条跳过是 **DEBUG** 日志（publisher.rs:2303 "skipping group with a
 * missing head"），默认 info 级别下**完全静默**，所以症状只是「对端偶尔少一个
 * 组」而看不出原因。
 *
 * 证据（`rs/moq-net/`）：
 *   - `lite/subscribe.rs:41` `carries_max_age() = !matches!(Lite01|Lite02)`
 *     —— **Lite06 携带这个字段**，所以发布端不会替我们兜底。
 *   - `lite/publisher.rs:77` 的 `serving_max_age`，单测写死：
 *       "A version that does carry it is taken at its word, zero included."
 *       assert_eq!(serving_max_age(Lite05, Duration::ZERO), Duration::ZERO);
 *     —— 发 0 就是真的按 0 服务。
 *   - `lite/subscriber.rs:3733` 的 `.filter(|max_age| !max_age.is_zero())`
 *     —— **参考实现的订阅端从不发 0**（0 时它干脆不写这个字段）。
 *   - `model/track.rs:35` `DEFAULT_MAX_AGE = Duration::from_secs(5)`。
 *
 * 实测代价：发 0 时音频少收约 3.5%（20 秒 938 帧只到 905），视频几乎不掉
 * （25fps 的组大、且组边界跟着关键帧走，游标不会擦到）；浏览器侧表现为每个
 * 缺口注 21ms 静音 → 音频继续播放但有周期性 pop。
 *
 * 5 秒 = 参考实现的默认值，也比我们自己的 GOP 回放窗口（2 秒）宽，够用。 */
constexpr uint64_t kSubMaxAgeMs = 5000;

/* ── M3：媒体落地 ── */

/* 容器前缀的时间单位。hang 的 legacy 容器把帧时间戳归一化到微秒写进
 * payload 开头（rs/hang/src/container/frame.rs:12 "no scale tag"、
 * `TIMESCALE = MICRO`），**与 track timescale 无关**。所以这一层是自描述的：
 * 外层 FRAME 头那个 zigzag 差值是 track timescale 单位（我们没取 TRACK_INFO，
 * 不知道单位），内层这个 varint 却恒是微秒 —— M3 以**内层**为准。
 * 两者本应相等（实测相等），不等就说明 timescale != 1e6，打一次警告。
 * 媒体层要毫秒（MediaStream::NormalizeDts 按 ms 平移），所以再除一次。 */
constexpr int64_t kUsPerMs = 1000;

/* 帧日志的组数上限 —— 与 M2 的 kFrameLogLimit 同源（R13：debug 级别下
 * 每包打日志能啃掉几百 MB，逐帧日志必须有闸）。 */
constexpr uint64_t kDeliverLogLimit = 3;

/* 落地计数的 INFO 间隔（帧）。见 OnMediaFrame 尾部：这条是 INFO 级别验收
 * 「帧进了 MediaStreamManager」的唯一凭据，不受 kDeliverLogLimit 的封顶影响。
 * 100 → 500：音频 47 帧/秒时原来是 ~2 行/秒，太吵；500 帧 ≈ 10.6 秒一行，
 * 仍然是可读的存活心跳（视频 30fps 时 ≈ 16.7 秒一行）。 */
constexpr uint64_t kDeliverLogEvery = 500;

/* 落地的流 key 的 app 段为空时的兜底。只有路由路径为空（理论上不该发生）
 * 才会走到，日志里会带 WARN。 */
constexpr const char *kFallbackApp = "moqlite";

/* 对端可以在 Message Length 里声明多大的消息 —— 定义已挪到 moq_lite_wire.hpp
 * （kMlMaxMessageSize），因为消息框的解码也在那儿。这里留个别名，本文件的
 * 语义不变：超出的长度字段本身就是违规，不必等它把字节发完。 */
constexpr uint64_t kMaxMessageSize = kMlMaxMessageSize;

/* 单条接收流的缓冲上限 = 最大消息 + 余量（长度字段自身、SETUP 的
 * Parameter Count、GROUP 的头等）。 */
constexpr size_t kMaxRecvBuf =
    static_cast<size_t>(kMaxMessageSize) + 1024;

/* -06 §7.3 SETUP 参数 ID */
constexpr uint64_t kParamProbe = 0x1;
constexpr uint64_t kParamPath  = 0x2;
constexpr uint64_t kParamRole  = 0x3;
constexpr uint64_t kParamCost  = 0x4;
constexpr uint64_t kParamHop   = 0x5;

const char *ParamName(uint64_t id) {
    switch (id) {
    case kParamProbe: return "Probe";
    case kParamPath:  return "Path";
    case kParamRole:  return "Role";
    case kParamCost:  return "Cost";
    case kParamHop:   return "Hop";
    default:          return "unknown";
    }
}

/* -06 §7.6~§7.8 Announce 族的 **Type 判别符**（在 Message Length **之外**、
 * 之前，即 [Type][Length][body]）。注意只有这三种消息带 Type ——
 * ANNOUNCE_REQUEST 与 ANNOUNCE_OK 是普通消息，上来就是长度。
 * rs/moq-net/src/lite/announce.rs:11-13（那里的 ANNOUNCE_RESTART 就是
 * 草案里的 ANNOUNCE_UPDATE，线上值相同）。 */
constexpr uint64_t kAnnTypeStart  = 0x0;
constexpr uint64_t kAnnTypeEnd    = 0x1;
constexpr uint64_t kAnnTypeUpdate = 0x2;

/* -06 §7.13~§7.15 Subscribe 的应答判别符。
 * ⚠️ **-06 线上没有 SUBSCRIBE_OK** —— 接受是隐式的（不 reset + 来数据）。
 * 草案仍把 0x0 叫 SUBSCRIBE_OK，参考实现改叫 SUBSCRIBE_START，同一个消息。
 * rs/moq-net/src/lite/subscribe.rs:490-497 是权威注释：
 *   "Lite05/06: 0x0 SUBSCRIBE_START, 0x1 SUBSCRIBE_END, 0x2 SUBSCRIBE_DROP
 *    (SUBSCRIBE_OK was removed; acceptance is implicit)" */
constexpr uint64_t kSubTypeStart = 0x0;
constexpr uint64_t kSubTypeEnd   = 0x1;
constexpr uint64_t kSubTypeDrop  = 0x2;

/* 订阅优先级：发布端「SHOULD 先发高值」。catalog 要先到（它是后面一切订阅的
 * 依据），音频比视频先到（rs/hang/src/catalog/priority.rs:21-26）：
 * catalog 100 / text 90 / audio 80 / video 60。 */
constexpr uint64_t kSubPriorityCatalog = 100;
constexpr uint64_t kSubPriorityAudio   = 80;
constexpr uint64_t kSubPriorityVideo   = 60;

/* subscribe id 是会话内唯一的 u64，我们这边从 1 起自己发号。 */

/* catalog 的 track 名是字面量 `catalog.json`（rs/hang/src/catalog/root.rs:107
 * 的 DEFAULT_NAME）—— 它不是特殊通道，就是 broadcast 里的一条普通 track，
 * 每次更新一个组、组里一帧 UTF-8 JSON。 */
constexpr const char *kCatalogTrack = "catalog.json";

/* 每条 track 最多为前几个组打一行帧形状日志。M2 要的是「看清线上长什么样」，
 * M3 要的是不把磁盘写满（R13）—— 所以这里是个硬上限，不是开关。 */
constexpr uint64_t kFrameLogLimit = 3;

/* (s) 路径拼接：**空的一侧不产生分隔符**，否则会造出 "/x" 这种前导斜杠，
 * 与参考实现的 Path::join 不一致（rs/moq-net/src/path/mod.rs:340-354）：
 *   if self.is_empty() { other } else if other.is_empty() { self }
 *   else { format!("{}/{}", self, other) }
 * 归一化后的路径两端都没有 '/'，所以非空时补一个就够。 */
std::string JoinPath(const std::string &a, const std::string &b) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    return a + "/" + b;
}

/* 小写 hex 解码。catalog 的 description 就是 hex（不带 0x、无分隔符，
 * 发布端出小写、消费端两种都收）。遇到非 hex 字符返回 false，
 * out 里已解出的部分不保证完整 —— 调用方失败即丢弃。 */
bool HexDecode(const std::string &in, std::vector<uint8_t> *out) {
    if (in.size() % 2 != 0) return false;
    out->clear();
    out->reserve(in.size() / 2);
    for (size_t i = 0; i < in.size(); i += 2) {
        int hi = 0, lo = 0;
        for (int k = 0; k < 2; k++) {
            const char c = in[i + static_cast<size_t>(k)];
            int v = -1;
            if (c >= '0' && c <= '9')      v = c - '0';
            else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
            if (v < 0) return false;
            if (k == 0) hi = v; else lo = v;
        }
        out->push_back(static_cast<uint8_t>((hi << 4) | lo));
    }
    return true;
}

/* 未知名返回 NULL —— 调用方要靠它区分「未知类型，必须 reset」。 */
const char *StreamTypeName(uint64_t type, int uni) {
    if (uni) {
        switch (type) {
        case kUniGroup: return "Group";
        case kUniSetup: return "Setup";
        default:        return nullptr;
        }
    }
    switch (type) {
    case kBidiAnnounce:  return "Announce";
    case kBidiSubscribe: return "Subscribe";
    case kBidiFetch:     return "Fetch";
    case kBidiProbe:     return "Probe";
    case kBidiGoaway:    return "Goaway";
    case kBidiTrack:     return "Track";
    default:             return nullptr;
    }
}

/* (s) —— 长度前缀串，用 MlReadString（长度本身也是 QUIC varint）。
 * 注意这根串外面通常还套着 Parameter Length，两层长度都在。 */

/* 路径归一化：去掉前导 '/'，便于和配置里的 subpath（写成 "moq" 这种）对齐。
 * 只去一个 —— "//x" 是另一个路径，不该被当成 "/x"。 */
std::string NormalizePath(const std::string &in) {
    if (!in.empty() && in[0] == '/') return in.substr(1);
    return in;
}

/* owner id 里的进程内序号。MediaStreamManager 用 owner 区分「谁在推这条流」，
 * 被顶掉的旧会话靠它认出来。只用 ip:port 不够：客户端重连时源端口可能复用，
 * 那时新会话会和旧会话撞成一个 id，顶替逻辑就看不见这次更换了。
 * 只在 QUIC loop 线程上取，不需要原子。 */
uint64_t NextSessionSerial() {
    static uint64_t serial = 0;
    return ++serial;
}

} /* namespace */

/* ============================================================
 * 注册表
 * ============================================================ */

std::map<QuicConnection *, MoqLiteSession *> &MoqLiteSession::Registry() {
    static std::map<QuicConnection *, MoqLiteSession *> r;
    return r;
}

/* ============================================================
 * 构造 / 析构
 * ============================================================ */

MoqLiteSession::MoqLiteSession(QuicConnection *conn,
                               std::string alpn,
                               std::string remote_ip, int remote_port,
                               OnRoute on_route, OnClose on_close,
                               OnStreamMissing on_stream_missing,
                               int deferred_subscribe_ms)
    : conn_(conn),
      alpn_(std::move(alpn)),
      remote_ip_(std::move(remote_ip)),
      remote_port_(remote_port),
      on_route_(std::move(on_route)),
      on_close_(std::move(on_close)),
      on_stream_missing_(std::move(on_stream_missing)),
      deferred_subscribe_ms_(deferred_subscribe_ms > 0 ? deferred_subscribe_ms
                                                       : 3000) {
    /* owner id 在构造时就定下来：它标识「这条连接」，与路由、broadcast 无关，
     * 所以在任何回调之前生成最省心。 */
    char buf[64];
    snprintf(buf, sizeof(buf), "moqlite-%llu",
             static_cast<unsigned long long>(NextSessionSerial()));
    owner_id_ = std::string(buf) + "/" + remote_ip_ + ":" + std::to_string(remote_port_);
}

MoqLiteSession::~MoqLiteSession() {
    /* 摘牌必须在 erase(conn_) 之前 —— 两者没有依赖，但把「先收拾对外状态、
     * 再收拾自己」固定下来，免得以后有人在中间插一句用到 conn_ 的代码。
     * 下行写入器也要摘：它们在 MediaStreamManager 的 writer_map_ 里挂着**裸
     * 指针**，本对象一析构就是悬垂 —— 下一个媒体包会让服务端崩在别人的
     * 回调里。Stop() 里对 QUIC 的写在这里已经打不通（连接在拆），但那是无害
     * 的：Push/Stop 都吞掉 -1。 */
    TeardownEgress("session destroyed");
    ReleaseStream();
    if (conn_) Registry().erase(conn_);
}

void MoqLiteSession::Start() {
    if (!conn_) return;
    Registry()[conn_] = this;
    QuicConnectionSetOnStreamData(conn_, &MoqLiteSession::OnStreamDataTrampoline);
    QuicConnectionSetOnClose(conn_, &MoqLiteSession::OnCloseTrampoline);
    SendSetup();
}

void MoqLiteSession::CloseSession(uint64_t error_code, const char *reason) {
    if (dead_ || !conn_) return;
    dead_ = true;
    LOG_INFO("[moq-lite] %s:%d closing session: err=%llu reason=%s",
             remote_ip_.c_str(), remote_port_,
             static_cast<unsigned long long>(error_code),
             reason ? reason : "");
    /* 先摘 publisher 牌子再断连：断开之后我们收不到任何回调，牌子挂在那里
     * 会让这条流一直显得「有 publisher」，后来者要等顶替流程才拿得到。 */
    ReleaseStream();
    QuicConnectionClose(conn_, error_code, reason ? reason : "");
}

/* ============================================================
 * 发送：自己的 Setup 流
 * ============================================================ */

void MoqLiteSession::SendSetup() {
    const uint64_t sid = QuicConnectionStreamOpenUni(conn_);
    if (sid == UINT64_MAX) {
        LOG_ERROR("[moq-lite] %s:%d open setup stream failed",
                  remote_ip_.c_str(), remote_port_);
        CloseSession(kSessionInternalError, "cannot open setup stream");
        return;
    }

    /* STREAM_TYPE(0x1) + SETUP{ Message Length(i); Parameter Count(i) }
     * Message Length 只覆盖它自己之后的字节（§7.1），所以空参数表时 = 1
     * （就是 Parameter Count 那个字节）。
     * 这三个值都 < 64，所以 vi64 与 QUIC varint 恰好逐字节相同 —— 但编码必须
     * 是 QUIC varint（R1 定案），这里用 MlWriteVarint 而不是 MoqAppendVi64。 */
    std::vector<uint8_t> out;
    if (!MlWriteVarint(out, kUniSetup) ||   /* STREAM_TYPE */
        !MlWriteVarint(out, 1) ||           /* Message Length */
        !MlWriteVarint(out, 0)) {           /* Parameter Count = 0 */
        CloseSession(kSessionInternalError, "cannot encode setup");
        return;
    }

    if (QuicConnectionStreamSend(conn_, sid, out.data(), out.size(), 1) < 0) {
        LOG_ERROR("[moq-lite] %s:%d send setup failed",
                  remote_ip_.c_str(), remote_port_);
        CloseSession(kSessionInternalError, "cannot send setup");
        return;
    }
    LOG_INFO("[moq-lite] %s:%d alpn=%s setup sent (%zu bytes, empty params)",
             remote_ip_.c_str(), remote_port_,
             alpn_.empty() ? "(none)" : alpn_.c_str(), out.size());
}

/* ============================================================
 * 接收分派
 * ============================================================ */

void MoqLiteSession::OnStreamDataTrampoline(QuicConnection *conn,
                                            uint64_t stream_id,
                                            const uint8_t *data, size_t len,
                                            int fin) {
    auto it = Registry().find(conn);
    if (it == Registry().end() || !it->second) return;
    it->second->HandleStreamData(stream_id, data, len, fin);
}

void MoqLiteSession::OnCloseTrampoline(QuicConnection *conn,
                                       uint64_t error_code,
                                       const char *reason) {
    /* listener 记账：交割之后这条连接的 on_close 归我们，http3_server 的
     * h3_on_close 不会再跑，它原来负责的 QuicListenerRemoveConn 得由这里补上。
     * 见 http3_server.h 的说明。 */
    void *l = QuicConnectionGetListener(conn);
    if (l) QuicListenerRemoveConn(static_cast<QuicListener *>(l), conn);

    auto it = Registry().find(conn);
    if (it == Registry().end() || !it->second) return;
    MoqLiteSession *sess = it->second;
    sess->dead_ = true;

    /* 先把回调拷出来再注销：on_close 里上层会把本对象 delete 掉，
     * 此后不能再碰 sess 的成员。 */
    OnClose cb = sess->on_close_;
    Registry().erase(it);
    LOG_INFO("[moq-lite] %s:%d session closed: err=%llu reason=%s",
             sess->remote_ip_.c_str(), sess->remote_port_,
             static_cast<unsigned long long>(error_code),
             reason ? reason : "");
    if (cb) cb(*sess);
}

void MoqLiteSession::HandleStreamData(uint64_t sid, const uint8_t *data,
                                      size_t len, int fin) {
    if (dead_) return;
    RecvStream &st = streams_[sid];
    if (st.reset_sent) return;

    if (data && len) st.buf.insert(st.buf.end(), data, data + len);
    if (fin) st.fin = 1;

    /* 无界缓冲守卫。对端在 Message Length 里声明一个巨大的消息、却不把字节
     * 发完，缓冲区就会一直涨到内存耗尽 —— 而它既没超时也没违规到能被解析器
     * 发现。超过「合法消息上限 + 余量」只能是这种情况，按**流**错误 reset：
     * 一条坏流不该把整个会话（以及上面其它订阅）带走。 */
    if (st.buf.size() > kMaxRecvBuf) {
        ResetStream(sid, kStreamInternalError, st,
                    "receive buffer over maximum message size");
        st.buf.clear();
        st.buf.shrink_to_fit();
        return;
    }

    Parse(sid, st);
}

void MoqLiteSession::Parse(uint64_t sid, RecvStream &st) {
    if (!st.typed) {
        size_t off = 0;
        uint64_t type = 0;
        if (!MlReadVarint(st.buf.data(), st.buf.size(), off, type)) {
            if (st.fin) {
                LOG_WARN("[moq-lite] %s:%d stream %llu ended inside STREAM_TYPE",
                         remote_ip_.c_str(), remote_port_,
                         static_cast<unsigned long long>(sid));
                ResetStream(sid, kStreamInternalError, st, "truncated stream type");
            }
            return;
        }
        st.type = type;
        st.typed = true;
        st.buf.erase(st.buf.begin(),
                     st.buf.begin() + static_cast<std::ptrdiff_t>(off));
        /* dead_ 也要查 —— OnStreamTypeSeen 可能在第二条 Setup 流上把会话关掉
         * （§6.3.1），那就没必要再往下解这条流了 */
        OnStreamTypeSeen(sid, st);
        if (st.reset_sent || dead_) return;
    }

    const int uni = (sid & kStreamIdUniBit) ? 1 : 0;
    if (st.local) {
        /* 本端发起的 bidi 控制流 —— 类型字节是我们自己写的（local 的流建流时
         * typed 已置真），对端在同一根流上回的**应答不带类型字节**，直接就是
         * 消息。少了这条分支，ANNOUNCE_OK 的首字节会被当成 STREAM_TYPE 解掉。 */
        if (st.type == kBidiAnnounce)       ParseAnnounceStream(sid, st);
        else if (st.type == kBidiSubscribe) ParseSubscribeStream(sid, st);
        else                                ParseBidiStream(sid, st);
    } else if (uni) {
        if (st.type == kUniSetup) ParseSetupStream(sid, st);
        else                       ParseGroupStream(sid, st);
    } else {
        /* 对端主动开的 bidi 流。bidi 的 creator 是 subscriber（-06 §5.1），
         * 所以这三条只可能是**对端的请求**，我们要在**同一条流上**应答 ——
         * 流 id 本身就是「哪个应答属于哪个请求」的关联键，不需要另做 ID 配对。
         * （我们自己开的 bidi 流不会走到这里：上面 st.local 那条先分走了。）*/
        if (st.type == kBidiAnnounce)       ParsePeerAnnounce(sid, st);
        else if (st.type == kBidiSubscribe) ParsePeerSubscribe(sid, st);
        else if (st.type == kBidiTrack)     ParsePeerTrack(sid, st);
        else if (st.type == kBidiFetch || st.type == kBidiProbe) {
            /* Fetch / Probe 本端不支持。**必须 reset，不能静默忽略** ——
             * 静默会让对端一直等一条永不到来的应答（按协议我们本来就该回，
             * 所以它没有理由超时）。错误码挑 CANCELLED：草案里没有「不支持」
             * 这一类，而「这条流上的请求被取消了」是语义上最接近的。 */
            ResetStream(sid, kStreamCancelled, st, "fetch/probe not supported");
            st.done = true;
            st.buf.clear();
            return;   /* reset 之后不再碰 st（见 Parse 尾部的警告）*/
        }
        else ParseBidiStream(sid, st);
    }

    /* 已收尾的流不再需要缓冲区；不丢的话对端继续写就会一直涨。
     *
     * ⚠️ 这里在分派之后还碰了 st（也就是 streams_ 里的元素），所以分派过程中
     * 不能把本对象销毁掉。当前成立：CloseSession 走的是 QuicConnectionClose，
     * 它只把连接置为 CLOSING 并起 1s 的 close_timer，on_close 不在本调用栈里
     * 触发。若将来改成同步回调，这一行和 Parse 的整个「分派后收尾」都要重做。 */
    if (st.done) st.buf.clear();
}

void MoqLiteSession::OnStreamTypeSeen(uint64_t sid, RecvStream &st) {
    const int uni = (sid & kStreamIdUniBit) ? 1 : 0;
    const char *name = StreamTypeName(st.type, uni);

    /* §7.2：未知流类型必须 reset 该流，且**不是**致命错误 ——
     * 这正是「扩展流开给了没协商该扩展的对端」时的兜底。 */
    if (!name) {
        LOG_WARN("[moq-lite] %s:%d peer opened stream %llu with unknown type %llu",
                 remote_ip_.c_str(), remote_port_,
                 static_cast<unsigned long long>(sid),
                 static_cast<unsigned long long>(st.type));
        ResetStream(sid, kStreamInternalError, st, "unknown stream type");
        return;
    }

    /* §6.3.1：每个方向只有一条 Setup 流，收到第二条 → PROTOCOL_VIOLATION。
     * 卡在「流类型刚看到」这一层，不等 SETUP 解析完 —— 否则第一条还没收全时
     * 来的第二条会漏判。 */
    if (uni && st.type == kUniSetup) {
        if (saw_peer_setup_) {
            CloseSession(kSessionProtocolViolation, "second setup stream");
            return;
        }
        saw_peer_setup_ = true;
    }

    /* 每条组流都会走这里：音频 1 组/秒、视频 1 组/关键帧 —— INFO 下就是稳定的
     * 每秒一两行噪声。组结构真正要看时切 debug。 */
    LOG_DEBUG("[moq-lite] %s:%d peer opened %s stream %llu type=0x%llx",
              remote_ip_.c_str(), remote_port_, name,
              static_cast<unsigned long long>(sid),
              static_cast<unsigned long long>(st.type));
}

void MoqLiteSession::ResetStream(uint64_t sid, uint64_t error_code,
                                 RecvStream &st, const char *why) {
    st.reset_sent = true;
    LOG_WARN("[moq-lite] %s:%d reset stream %llu: err=%llu (%s)",
             remote_ip_.c_str(), remote_port_,
             static_cast<unsigned long long>(sid),
             static_cast<unsigned long long>(error_code), why ? why : "");
    QuicConnectionStreamReset(conn_, sid, error_code);
}

/* ============================================================
 * Setup 流
 * ============================================================ */

void MoqLiteSession::ParseSetupStream(uint64_t sid, RecvStream &st) {
    (void)sid;
    if (st.done) return;   /* 第二条 Setup 流在上游 OnStreamTypeSeen 就拦掉了 */

    const uint8_t *p = st.buf.data();
    const size_t   n = st.buf.size();

    size_t off = 0;
    uint64_t msg_len = 0;
    if (!MlReadVarint(p, n, off, msg_len)) {
        if (st.fin) CloseSession(kSessionProtocolViolation, "truncated setup");
        return;                                  /* 还没到齐 */
    }
    const size_t len_field = off;                 /* 长度字段自身占的字节数 */
    if (n < len_field + static_cast<size_t>(msg_len)) {
        if (st.fin) CloseSession(kSessionProtocolViolation, "truncated setup body");
        return;
    }
    const size_t end = len_field + static_cast<size_t>(msg_len);

    saw_peer_setup_ = true;

    size_t pos = len_field;
    uint64_t param_count = 0;
    if (!MlReadVarint(p, end, pos, param_count)) {
        CloseSession(kSessionProtocolViolation, "setup: bad parameter count");
        return;
    }

    bool ok = true;
    std::string bad;
    std::vector<uint64_t> seen;
    for (uint64_t i = 0; ok && i < param_count; i++) {
        uint64_t pid = 0, plen = 0;
        if (!MlReadVarint(p, end, pos, pid) ||
            !MlReadVarint(p, end, pos, plen) ||
            plen > end - pos) {
            ok = false;
            bad = "setup: parameter overruns message";
            break;
        }
        /* §7.3：同一个 Parameter ID 出现两次 → PROTOCOL_VIOLATION */
        for (size_t k = 0; k < seen.size(); k++) {
            if (seen[k] == pid) {
                ok = false;
                bad = "setup: duplicate parameter id";
                break;
            }
        }
        if (!ok) break;
        seen.push_back(pid);
        OnSetupParam(pid, p + pos, static_cast<size_t>(plen), &ok, &bad);
        pos += static_cast<size_t>(plen);
    }

    /* §7.1：长度字段与实际内容不符也要 PROTOCOL_VIOLATION */
    if (ok && pos != end) {
        ok = false;
        bad = "setup: message length mismatch";
    }
    if (!ok) {
        CloseSession(kSessionProtocolViolation, bad.c_str());
        return;
    }

    /* 未知参数已经忽略掉了，剩余字节也按 §7.1 的「长度必须对得上」处理完了 */
    st.buf.erase(st.buf.begin(), st.buf.begin() + static_cast<std::ptrdiff_t>(end));
    st.done = true;

    peer_setup_done_ = true;
    LOG_INFO("[moq-lite] %s:%d peer setup: %llu param(s) path=\"%s\"",
             remote_ip_.c_str(), remote_port_,
             static_cast<unsigned long long>(param_count),
             path_raw_.c_str());

    if (on_route_ && !on_route_(*this)) {
        /* §7.3.2：不认识请求的路径必须关闭会话 */
        CloseSession(kSessionUnauthorized, "unknown path");
        return;
    }

    /* 路由定了 —— 按**对端的角色**决定往哪个方向走。Role 是「对端自己是谁」：
     *
     *   publisher(1)  → 对端推流，我们订阅（M2/M3 现状，字节不变）
     *   subscriber(2) → 对端只想拉流，我们当发布端应答（M4）
     *   both(0)/未知   → 两个方向都做
     *
     * ⚠️ 对端 = subscriber 时**绝不能**发 ANNOUNCE_REQUEST：那是订阅方的动作，
     * 一个只想订阅的对端收到它只会把那条流 reset 掉（§5.1 的 scope 规则里
     * 没有任何一条要求它应答订阅请求）。两件事互不干扰，顺序也无所谓。 */
    if (peer_role_ == 1) {
        LOG_INFO("[moq-lite] %s:%d peer Role=publisher: subscribing (M2/M3 path)",
                 remote_ip_.c_str(), remote_port_);
        StartDiscovery();
    } else if (peer_role_ == 2) {
        LOG_INFO("[moq-lite] %s:%d peer Role=subscriber: serving (M4 path)",
                 remote_ip_.c_str(), remote_port_);
        ArmPublisher();
    } else {
        LOG_INFO("[moq-lite] %s:%d peer Role=both: subscribing and serving",
                 remote_ip_.c_str(), remote_port_);
        StartDiscovery();
        ArmPublisher();
    }
}

void MoqLiteSession::ArmPublisher() {
    if (publisher_armed_ || dead_) return;
    publisher_armed_ = true;
    /* 与 StartDiscovery 相反：这里**什么都不发**。发布端是纯应答方 ——
     * 对端开一条 announce/subscribe/track 流过来，我们才在**同一条流上**回。
     * 主动发只会打扰一个没在等的对端（§5.1：每条请求自己占一条 bidi 流，
     * 应答在同一条流上回来，所以流 id 本身就是关联键，不需要我们先打招呼）。
     *
     * ⚠️ 已知缺口：ANNOUNCE 只答**当时的**快照（AnnounceGrid）。**拉流端先连、
     * 推流端后起**时，那条已经开着的 announce 流收不到任何后续 ANNOUNCE_START，
     * 对端会一直看不到这条广播。补法是决策 8 的服务级回调
     * （MoqLiteServer 注册一个 StreamManagerCallbackI，OnPublish 时扇出到各会话
     * 的 announce 流上）—— 尚未接线。推流先起、拉流后连的路径不受影响。 */
    LOG_INFO("[moq-lite] %s:%d publisher role armed: awaiting peer announce/"
             "subscribe/track streams",
             remote_ip_.c_str(), remote_port_);
}

void MoqLiteSession::OnSetupParam(uint64_t id, const uint8_t *val, size_t len,
                                  bool *bad_format, std::string *bad_reason) {
    size_t off = 0;
    uint64_t v = 0;

    switch (id) {
    case kParamPath: {
        /* §7.3.2：值 = path-abempty，带 query 时追加 "?query"。空值等价于没发。
         * 这条参数只有客户端发；我们是服务端，收到是对的，发才是违规。 */
        std::string s;
        if (!MlReadString(val, len, off, s) || off != len) {
            *bad_format = true;
            *bad_reason = "setup: malformed Path parameter";
            return;
        }
        path_raw_ = s;
        const size_t q = s.find('?');
        if (q == std::string::npos) {
            path_only_ = NormalizePath(s);
        } else {
            path_only_ = NormalizePath(s.substr(0, q));
            query_     = s.substr(q + 1);
        }
        break;
    }
    case kParamProbe:
        if (!MlReadVarint(val, len, off, v)) {
            *bad_format = true;
            *bad_reason = "setup: malformed Probe parameter";
            return;
        }
        LOG_DEBUG("[moq-lite] %s:%d setup Probe level=%llu",
                  remote_ip_.c_str(), remote_port_,
                  static_cast<unsigned long long>(v));
        break;
    case kParamRole:
        if (!MlReadVarint(val, len, off, v)) {
            *bad_format = true;
            *bad_reason = "setup: malformed Role parameter";
            return;
        }
        /* 0 Both（默认）/ 1 Publisher / 2 Subscriber；不认识的按 Both 处理。
         * 归一化后再存：后面 ParseSetupStream 的分支只认这三个值，留个 7
         * 在那里等于让每个读它的地方都自己判一遍。 */
        peer_role_ = (v == 1 || v == 2) ? v : 0;
        LOG_INFO("[moq-lite] %s:%d setup Role=%llu (%s)",
                 remote_ip_.c_str(), remote_port_,
                 static_cast<unsigned long long>(v),
                 peer_role_ == 1 ? "publisher"
                                 : (peer_role_ == 2 ? "subscriber" : "both"));
        break;
    case kParamCost:
    case kParamHop:
        if (!MlReadVarint(val, len, off, v)) {
            *bad_format = true;
            *bad_reason = "setup: malformed cost/hop parameter";
            return;
        }
        LOG_DEBUG("[moq-lite] %s:%d setup %s=%llu",
                  remote_ip_.c_str(), remote_port_, ParamName(id),
                  static_cast<unsigned long long>(v));
        break;
    default:
        /* §7.3：未知 Parameter ID 必须忽略 */
        LOG_DEBUG("[moq-lite] %s:%d setup param id=0x%llx len=%zu ignored",
                  remote_ip_.c_str(), remote_port_,
                  static_cast<unsigned long long>(id), len);
        break;
    }
}

/* ============================================================
 * M2 —— announce / subscribe / group
 *
 * 结构（-06 §5.1）：**每条请求自己占一条 bidi 流，应答在同一条流上回来**，
 * 所以「哪个应答属于哪个请求」不需要 ID 配对 —— stream id 本身就是关联键。
 * 只有 GROUP 流（对端单向新开的）要靠 Subscribe ID 反查。
 * ============================================================ */

void MoqLiteSession::StartDiscovery() {
    if (discovery_started_ || dead_) return;
    discovery_started_ = true;

    /* R8：ANNOUNCE_REQUEST 该带哪个 prefix **反推不出来** —— 结论 17 实测
     * moq-cli 的 SETUP 不带 Path 参数，`/anon` 根本不在线上，服务端拿不到任何
     * 线索。而发错的后果是**静默的**：请求前缀不在发布端 scope 内时，它回一个
     * active=0 的空集合、保持流打开、不报错（-06 §5.1.1；参考实现
     * rs/lite/publisher.rs:543-566 的注释就写着「rather than erroring, which
     * would reset the stream」）。
     *
     * 所以这里两条都发：一条空 prefix（语义是「这个 scope 下的全部」），一条
     * routed_path_（「路径那一层也算进 broadcast 名」）。谁先回报非空就用谁，
     * 两个结果都进日志 —— 一轮实测就能定案，不用重编译。
     * 空集合是**合法结果**，不是错误，下面不会因此关会话。 */
    StartAnnounce("");
    if (!routed_path_.empty()) StartAnnounce(routed_path_);
}

/* 本端发起一条 bidi 控制流：开流 + 写 STREAM_TYPE。
 * 类型字节**只有发起方写**（-06 §5.1：bidi 流的 creator 是 subscriber），
 * 所以写完这条流就轮到对端在我们的请求上做应答了。 */
bool MoqLiteSession::OpenCtrlStream(uint64_t stream_type, uint64_t *out_sid) {
    const uint64_t sid = QuicConnectionStreamOpen(conn_);
    if (sid == UINT64_MAX) {
        LOG_ERROR("[moq-lite] %s:%d cannot open bidi stream for type %llu",
                  remote_ip_.c_str(), remote_port_,
                  static_cast<unsigned long long>(stream_type));
        return false;
    }

    std::vector<uint8_t> t;
    if (!MlWriteVarint(t, stream_type) ||
        QuicConnectionStreamSend(conn_, sid, t.data(), t.size(), 0) < 0) {
        LOG_ERROR("[moq-lite] %s:%d cannot write stream type %llu on %llu",
                  remote_ip_.c_str(), remote_port_,
                  static_cast<unsigned long long>(stream_type),
                  static_cast<unsigned long long>(sid));
        QuicConnectionStreamReset(conn_, sid, kStreamInternalError);
        return false;
    }
    *out_sid = sid;
    return true;
}

/* 把一条 [Type?][Message Length][body] 写在这条流上。fin=0 —— 控制流要一直
 * 开着等应答，**不能**顺手 FIN 掉。 */
bool MoqLiteSession::WriteMessage(uint64_t sid, bool typed, uint64_t type,
                                  const std::vector<uint8_t> &body, bool fin) {
    std::vector<uint8_t> out;
    if (!MlBuildMessage(out, typed, type, body)) {
        LOG_ERROR("[moq-lite] %s:%d cannot encode message type=0x%llx body=%zu",
                  remote_ip_.c_str(), remote_port_,
                  static_cast<unsigned long long>(type), body.size());
        return false;
    }
    if (QuicConnectionStreamSend(conn_, sid, out.data(), out.size(),
                                 fin ? 1 : 0) < 0) {
        LOG_ERROR("[moq-lite] %s:%d cannot send message type=0x%llx on %llu",
                  remote_ip_.c_str(), remote_port_,
                  static_cast<unsigned long long>(type),
                  static_cast<unsigned long long>(sid));
        return false;
    }
    return true;
}

void MoqLiteSession::StartAnnounce(const std::string &prefix) {
    uint64_t sid = 0;
    if (!OpenCtrlStream(kBidiAnnounce, &sid)) return;

    RecvStream &st = streams_[sid];
    st.local    = true;              /* 我们开的流：应答不带 STREAM_TYPE */
    st.typed    = true;
    st.type     = kBidiAnnounce;
    st.stage    = kAnnStageWaitOk;
    st.req_path = prefix;

    /* ANNOUNCE_REQUEST 是**普通消息**（没有 Type 判别符）：
     *   [Message Length(i)][Broadcast Path Prefix(s)]
     * -06 上**只有这两个字段**。exclude_hop 是 04/05 专有、hidden 是 07 专有，
     * 版本门控里对 -06 都编译掉了（rs/lite/announce.rs:414-424）。多写一个字节
     * 不是「多带个默认值」，是把长度和字段对齐一起搞坏。
     * 前缀是**绝对**路径，不是相对 scope 的片段。 */
    std::vector<uint8_t> body;
    if (!MlWriteString(body, prefix) || !WriteMessage(sid, false, 0, body, false)) {
        ResetStream(sid, kStreamInternalError, st, "cannot send announce request");
        return;
    }
    LOG_INFO("[moq-lite] %s:%d ANNOUNCE_REQUEST sent: prefix=\"%s\" stream=%llu",
             remote_ip_.c_str(), remote_port_, prefix.c_str(),
             static_cast<unsigned long long>(sid));
}

void MoqLiteSession::OnAnnounceRoute(const std::string &prefix,
                                     const std::string &suffix) {
    /* 全路径 = 请求前缀 + 路由后缀，中间补一个 '/'（空的一侧不补 —— 见
     * JoinPath）。后缀为空表示「发布端公告的就是请求的那个前缀本身」。 */
    const std::string full = JoinPath(prefix, suffix);

    if (!broadcast_.empty()) {
        LOG_INFO("[moq-lite] %s:%d another route announced: \"%s\" "
                 "(already using \"%s\")",
                 remote_ip_.c_str(), remote_port_, full.c_str(),
                 broadcast_.c_str());
        return;
    }

    broadcast_ = full;
    LOG_INFO("[moq-lite] %s:%d broadcast discovered: \"%s\" "
             "(prefix=\"%s\" suffix=\"%s\")",
             remote_ip_.c_str(), remote_port_, full.c_str(), prefix.c_str(),
             suffix.c_str());

    /* M3 的落点 key 到这里才齐（要 routed_path_ 与 broadcast_ 两个）。
     * 早于第一次订阅，所以媒体帧来时 stream_key_ 已经可用。 */
    BuildStreamKey();

    /* catalog 是**普通订阅**，没有专用通道：一条叫 catalog.json 的 track，
     * 每次更新一个组、组里一帧 UTF-8 JSON。后面每条媒体 track 的名字都要从
     * 它里面读，所以它是第一个订阅。 */
    if (!catalog_subscribed_) {
        catalog_subscribed_ = true;
        Subscribe(broadcast_, "catalog.json", kSubPriorityCatalog);
    }
}

void MoqLiteSession::ParseAnnounceStream(uint64_t sid, RecvStream &st) {
    /* 阶段 1：ANNOUNCE_OK{ Message Length(i); Hop ID(i); Active Count(i) }
     *
     * ⚠️ 字段顺序是 **Hop ID 在前、Active Count 在后**（草案 §7.5；
     * rs/lite/announce.rs:523-530），反了的话 active 会读成 hop。
     * Hop ID 是发布端自己的 ID，隐式地是每条路由 hop 列表的收尾项 ——
     * 所以它**不会**再出现在 ANNOUNCE_START 的 hop 列表里。 */
    if (st.stage == kAnnStageWaitOk) {
        size_t off = 0, blen = 0;
        const MlMsgStatus s = MlReadMessage(st.buf.data(), st.buf.size(), off, blen);
        if (s == kMlMsgTooLarge) {
            ResetStream(sid, kStreamInternalError, st, "announce ok over max message size");
            return;
        }
        if (s == kMlMsgNeedMore) {
            if (st.fin) {
                /* 对端的 FIN 在这条流上是**正常收尾**（发布端没什么可公告的
                 * 了），参考实现明确不把它升级成会话错误
                 * （rs/lite/subscriber.rs:1282-1291）。 */
                LOG_INFO("[moq-lite] %s:%d announce stream %llu closed by peer "
                         "before ANNOUNCE_OK (prefix=\"%s\", clean end)",
                         remote_ip_.c_str(), remote_port_,
                         static_cast<unsigned long long>(sid), st.req_path.c_str());
                st.done = true;
            }
            return;
        }

        const size_t end = off + blen;
        const uint8_t *p = st.buf.data();
        size_t pos = off;
        uint64_t hop = 0, active = 0;
        if (!MlReadVarint(p, end, pos, hop) ||
            !MlReadVarint(p, end, pos, active) || pos != end) {
            CloseSession(kSessionProtocolViolation, "announce: malformed ANNOUNCE_OK");
            return;
        }
        st.stage  = kAnnStageRunning;
        st.expect = active;
        st.buf.erase(st.buf.begin(), st.buf.begin() + static_cast<std::ptrdiff_t>(end));

        LOG_INFO("[moq-lite] %s:%d ANNOUNCE_OK: hop=%llu active=%llu "
                 "prefix=\"%s\" stream=%llu",
                 remote_ip_.c_str(), remote_port_,
                 static_cast<unsigned long long>(hop),
                 static_cast<unsigned long long>(active),
                 st.req_path.c_str(), static_cast<unsigned long long>(sid));
        if (active == 0) {
            /* 合法结果，但**必须能被看见** —— 否则「prefix 发错了」和「客户端
             * 确实没有 broadcast」在日志里长得一模一样（R10）。 */
            LOG_WARN("[moq-lite] %s:%d announce returned an EMPTY set for "
                     "prefix=\"%s\" (peer has nothing under this prefix)",
                     remote_ip_.c_str(), remote_port_, st.req_path.c_str());
        }
    }

    /* 阶段 2：带 Type 的消息 [Type][Message Length][body] */
    for (;;) {
        size_t toff = 0;
        uint64_t type = 0;
        if (!MlReadVarint(st.buf.data(), st.buf.size(), toff, type)) break;
        size_t boff = toff, blen = 0;
        const MlMsgStatus s = MlReadMessage(st.buf.data(), st.buf.size(), boff, blen);
        if (s == kMlMsgTooLarge) {
            ResetStream(sid, kStreamInternalError, st, "announce message over max size");
            return;
        }
        if (s == kMlMsgNeedMore) break;

        const size_t end = boff + blen;
        bool bad = false;
        std::string why;
        ParseAnnounceBody(st, type, st.buf.data() + boff, blen, &bad, &why);
        st.buf.erase(st.buf.begin(), st.buf.begin() + static_cast<std::ptrdiff_t>(end));
        if (bad) {
            CloseSession(kSessionProtocolViolation, why.c_str());
            return;
        }
    }

    if (st.fin && !st.done) {
        LOG_INFO("[moq-lite] %s:%d announce stream %llu closed by peer "
                 "(prefix=\"%s\", clean end)",
                 remote_ip_.c_str(), remote_port_,
                 static_cast<unsigned long long>(sid), st.req_path.c_str());
        st.done = true;
    }
}

void MoqLiteSession::ParseAnnounceBody(RecvStream &st, uint64_t type,
                                       const uint8_t *body, size_t len,
                                       bool *bad, std::string *why) {
    size_t pos = 0;

    if (type == kAnnTypeStart) {
        /* ANNOUNCE_START{ Type=0x0; Message Length(i); Route Prefix Suffix(s);
         *                 Hop Count(i); Hop ID(i)…; Warm Route Cost(i);
         *                 Cold Route Cost(i) }
         * ⚠️ 代价顺序是 **warm 在前、cold 在后**（rs/lite/announce.rs:194-195），
         * 参考实现的测试特意用不对称的 (12, 30) 就是防这个写反。 */
        std::string suffix;
        uint64_t hop_count = 0;
        if (!MlReadString(body, len, pos, suffix) ||
            !MlReadVarint(body, len, pos, hop_count)) {
            *bad = true;
            *why = "announce: malformed ANNOUNCE_START";
            return;
        }
        /* MAX_HOPS = 32（rs/model/origin.rs:195），超了就是解不出来 */
        uint64_t hops[32];
        if (hop_count > 32) {
            *bad = true;
            *why = "announce: hop count over maximum";
            return;
        }
        for (uint64_t i = 0; i < hop_count; i++) {
            if (!MlReadVarint(body, len, pos, hops[i])) {
                *bad = true;
                *why = "announce: malformed hop list";
                return;
            }
        }
        uint64_t warm = 0, cold = 0;
        if (!MlReadVarint(body, len, pos, warm) ||
            !MlReadVarint(body, len, pos, cold)) {
            *bad = true;
            *why = "announce: malformed route cost";
            return;
        }
        /* §7.1：长度字段与实际内容不符是 PROTOCOL_VIOLATION */
        if (pos != len) {
            *bad = true;
            *why = "announce: ANNOUNCE_START length mismatch";
            return;
        }

        /* 隐式 ID：线上**不带**，两端按这条流上的消息顺序各自数（每条 START
         * 递一个）。ANNOUNCE_END / UPDATE 用这个 ID 回指某条路由。 */
        const uint64_t id = st.ann_next_id++;
        if (st.expect > 0) st.expect--;
        LOG_INFO("[moq-lite] %s:%d ANNOUNCE_START id=%llu prefix=\"%s\" "
                 "suffix=\"%s\" hops=%llu cost(warm=%llu cold=%llu) "
                 "initial_left=%llu",
                 remote_ip_.c_str(), remote_port_,
                 static_cast<unsigned long long>(id), st.req_path.c_str(),
                 suffix.c_str(), static_cast<unsigned long long>(hop_count),
                 static_cast<unsigned long long>(warm),
                 static_cast<unsigned long long>(cold),
                 static_cast<unsigned long long>(st.expect));
        OnAnnounceRoute(st.req_path, suffix);
        return;
    }

    if (type == kAnnTypeEnd) {
        /* ANNOUNCE_END{ Type=0x1; Message Length(i); Announce ID(i) } —— 撤回路由。
         * 未知 / 已退休的 ID 是会话级违规（草案变更日志 2585-2588）。 */
        uint64_t id = 0;
        if (!MlReadVarint(body, len, pos, id) || pos != len) {
            *bad = true;
            *why = "announce: malformed ANNOUNCE_END";
            return;
        }
        if (id >= st.ann_next_id) {
            *bad = true;
            *why = "announce: ANNOUNCE_END references an unknown announce id";
            return;
        }
        LOG_INFO("[moq-lite] %s:%d ANNOUNCE_END id=%llu prefix=\"%s\"",
                 remote_ip_.c_str(), remote_port_,
                 static_cast<unsigned long long>(id), st.req_path.c_str());
        return;
    }

    if (type == kAnnTypeUpdate) {
        /* ANNOUNCE_UPDATE{ Type=0x2; Message Length(i); Announce ID(i);
         *                 Hop Count(i); Hop ID(i)…; Warm(i); Cold(i) }
         * ID **保持有效**，不递增计数器。发布端只改代价时也发这个。 */
        uint64_t id = 0, hop_count = 0;
        if (!MlReadVarint(body, len, pos, id) ||
            !MlReadVarint(body, len, pos, hop_count) || hop_count > 32) {
            *bad = true;
            *why = "announce: malformed ANNOUNCE_UPDATE";
            return;
        }
        if (id >= st.ann_next_id) {
            *bad = true;
            *why = "announce: ANNOUNCE_UPDATE references an unknown announce id";
            return;
        }
        uint64_t v = 0;
        for (uint64_t i = 0; i < hop_count; i++) {
            if (!MlReadVarint(body, len, pos, v)) {
                *bad = true;
                *why = "announce: malformed update hop list";
                return;
            }
        }
        uint64_t warm = 0, cold = 0;
        if (!MlReadVarint(body, len, pos, warm) ||
            !MlReadVarint(body, len, pos, cold) || pos != len) {
            *bad = true;
            *why = "announce: malformed ANNOUNCE_UPDATE cost";
            return;
        }
        LOG_INFO("[moq-lite] %s:%d ANNOUNCE_UPDATE id=%llu hops=%llu "
                 "cost(warm=%llu cold=%llu)",
                 remote_ip_.c_str(), remote_port_,
                 static_cast<unsigned long long>(id),
                 static_cast<unsigned long long>(hop_count),
                 static_cast<unsigned long long>(warm),
                 static_cast<unsigned long long>(cold));
        return;
    }

    /* 未知类型**按长度跳过**，不打断流 —— 旧实现对不了新消息，但它不该因此
     * 把整条流带走（rs/lite/announce.rs:300-306 的 Skipped）。 */
    LOG_DEBUG("[moq-lite] %s:%d announce: unknown message type %llu (%zu bytes) "
              "skipped", remote_ip_.c_str(), remote_port_,
              static_cast<unsigned long long>(type), len);
}

void MoqLiteSession::Subscribe(const std::string &broadcast,
                               const std::string &track, uint64_t priority) {
    uint64_t sid = 0;
    if (!OpenCtrlStream(kBidiSubscribe, &sid)) return;

    const uint64_t sub_id = next_subscribe_id_++;

    RecvStream &st = streams_[sid];
    st.local     = true;
    st.typed     = true;
    st.type      = kBidiSubscribe;
    st.stage     = kSubStageWaitResp;
    st.sub_id    = sub_id;
    st.req_path  = broadcast;
    st.req_track = track;

    /* **先登记再上线**：发布端可能在应答之前就把第一条 GROUP 开出来，那条流的
     * Subscribe ID 必须已经查得到 —— 参考实现特意把 insert 放在 flush 之前，
     * 注释就写着「a late insert races the group stream」（rs/lite/subscriber.rs
     * :3073-3080）。 */
    subs_[sub_id] = track;
    if (track == kCatalogTrack) {
        catalog_sub_id_ = sub_id;
    } else {
        auto tit = tracks_.find(track);
        if (tit != tracks_.end()) {
            tit->second.subscribe_id = sub_id;
            tit->second.started = false;
        }
    }

    /* SUBSCRIBE{ Message Length(i); Subscribe ID(i); Broadcast Path(s);
     *           Track Name(s); Subscriber Priority(8); Subscriber Max Age(i);
     *           Group Start(i); Group End(i); Frame Start(i); Frame End(i) }
     *
     * 三处 06 专属易错点：
     *  ① Priority 是**单字节 u8**，不是 varint；
     *  ② 06 **没有** Group Order 那个字节 —— 它是 01~05 专有，位置在 Priority
     *     与 Max Age 之间。多发一个字节，后面每个字段都错位（-06 变更日志：
     *     "a lite-06 peer cannot parse a lite-05 one's SUBSCRIBE"）；
     *  ③ Frame Start/End 是 06 **新增**的，必须发。
     * 四个范围字段的「不限定」编码**不一样**：Group Start / Frame Start 是裸
     * 绝对下标、0 就是不设限；Group End / Frame End 才是 0 = 不限定、非 0 =
     * 绝对末端 + 1。这里全 0 = 整条 track、不限组、不限帧。 */
    std::vector<uint8_t> body;
    if (!MlWriteVarint(body, sub_id) || !MlWriteString(body, broadcast) ||
        !MlWriteString(body, track)) {
        ResetStream(sid, kStreamInternalError, st, "cannot encode subscribe");
        return;
    }
    body.push_back(static_cast<uint8_t>(priority > 0xff ? 0xff : priority));
    /* ⚠️ 这里**不能**发 0 —— 0 是「零容忍」不是「不设上限」，见 kSubMaxAgeMs
     * 上方的长注释。下面四个范围字段才是 0 = 不限定。 */
    MlAppendVarint(body, kSubMaxAgeMs);  /* Subscriber Max Age = 5s 容忍度 */
    MlAppendVarint(body, 0);   /* Group Start = 0（无下限）*/
    MlAppendVarint(body, 0);   /* Group End = 0（无上限）*/
    MlAppendVarint(body, 0);   /* Frame Start = 0（整组）*/
    MlAppendVarint(body, 0);   /* Frame End = 0（整组）*/

    if (!WriteMessage(sid, false, 0, body, false)) {
        ResetStream(sid, kStreamInternalError, st, "cannot send subscribe");
        return;
    }
    LOG_INFO("[moq-lite] %s:%d SUBSCRIBE sent: id=%llu broadcast=\"%s\" "
             "track=\"%s\" priority=%llu max_age=%llums stream=%llu",
             remote_ip_.c_str(), remote_port_,
             static_cast<unsigned long long>(sub_id), broadcast.c_str(),
             track.c_str(), static_cast<unsigned long long>(priority),
             static_cast<unsigned long long>(kSubMaxAgeMs),
             static_cast<unsigned long long>(sid));
}

void MoqLiteSession::ParseSubscribeStream(uint64_t sid, RecvStream &st) {
    /* 应答是带 Type 的：[Type][Message Length][body]
     *   0x0 START{ Group(i) }       —— 接受。**没有 SUBSCRIBE_OK**，接受是隐式的
     *   0x1 END  { Group(i) }       —— 排他上界；-06 上**没有** Streams 字段
     *                                  （那是 07 专有，多读一个 varint 就是错）
     *   0x2 DROP { Group Start(i); Group End(i, 含); Error Code(i) }
     * 拒绝**不是**消息，是对端 reset 这条流（见函数末尾的说明）。 */
    for (;;) {
        size_t toff = 0;
        uint64_t type = 0;
        if (!MlReadVarint(st.buf.data(), st.buf.size(), toff, type)) break;
        size_t boff = toff, blen = 0;
        const MlMsgStatus s = MlReadMessage(st.buf.data(), st.buf.size(), boff, blen);
        if (s == kMlMsgTooLarge) {
            ResetStream(sid, kStreamInternalError, st, "subscribe response over max size");
            return;
        }
        if (s == kMlMsgNeedMore) break;

        const size_t end = boff + blen;
        const uint8_t *body = st.buf.data() + boff;
        size_t pos = 0;
        bool ok = true;
        uint64_t a = 0, b = 0, c = 0;

        if (type == kSubTypeStart) {
            ok = MlReadVarint(body, blen, pos, a) && pos == blen;
            if (ok) {
                st.stage = kSubStageRunning;
                auto tit = tracks_.find(st.req_track);
                if (tit != tracks_.end()) tit->second.started = true;
                LOG_INFO("[moq-lite] %s:%d SUBSCRIBE_START: track=\"%s\" "
                         "broadcast=\"%s\" id=%llu first_group=%llu",
                         remote_ip_.c_str(), remote_port_, st.req_track.c_str(),
                         st.req_path.c_str(),
                         static_cast<unsigned long long>(st.sub_id),
                         static_cast<unsigned long long>(a));
            }
        } else if (type == kSubTypeEnd) {
            ok = MlReadVarint(body, blen, pos, a) && pos == blen;
            if (ok) {
                LOG_INFO("[moq-lite] %s:%d SUBSCRIBE_END: track=\"%s\" id=%llu "
                         "end_group=%llu (exclusive, track done)",
                         remote_ip_.c_str(), remote_port_, st.req_track.c_str(),
                         static_cast<unsigned long long>(st.sub_id),
                         static_cast<unsigned long long>(a));
                st.done = true;
            }
        } else if (type == kSubTypeDrop) {
            ok = MlReadVarint(body, blen, pos, a) &&
                 MlReadVarint(body, blen, pos, b) &&
                 MlReadVarint(body, blen, pos, c) && pos == blen;
            if (ok) {
                /* 与 SUBSCRIBE 的 Group End 不同：这里的 Group End 是**含**的。
                 * start=5 end=7 表示 5、6、7 三组都丢了。 */
                LOG_INFO("[moq-lite] %s:%d SUBSCRIBE_DROP: track=\"%s\" id=%llu "
                         "groups %llu..%llu (inclusive) error=%llu",
                         remote_ip_.c_str(), remote_port_, st.req_track.c_str(),
                         static_cast<unsigned long long>(st.sub_id),
                         static_cast<unsigned long long>(a),
                         static_cast<unsigned long long>(b),
                         static_cast<unsigned long long>(c));
            }
        } else {
            LOG_DEBUG("[moq-lite] %s:%d subscribe stream %llu: unknown response "
                      "type %llu (%zu bytes) skipped",
                      remote_ip_.c_str(), remote_port_,
                      static_cast<unsigned long long>(sid),
                      static_cast<unsigned long long>(type), blen);
        }

        st.buf.erase(st.buf.begin(), st.buf.begin() + static_cast<std::ptrdiff_t>(end));
        if (!ok) {
            CloseSession(kSessionProtocolViolation, "subscribe: malformed response");
            return;
        }
        if (st.done) break;
    }

    if (st.fin && !st.done) {
        LOG_INFO("[moq-lite] %s:%d subscribe stream %llu closed by peer: "
                 "track=\"%s\" id=%llu",
                 remote_ip_.c_str(), remote_port_,
                 static_cast<unsigned long long>(sid), st.req_track.c_str(),
                 static_cast<unsigned long long>(st.sub_id));
        st.done = true;
    }
}

/* ============================================================
 * catalog
 * ============================================================ */

void MoqLiteSession::OnCatalogFrame(const uint8_t *payload, size_t len) {
    /* payload 里可能还垫着一层**容器时间戳前缀**（绝对微秒的 varint），也可能
     * 没有 —— 媒体帧那一层是结论 11 从源码定下来的，catalog 是数据 track，
     * 本轮没有权威结论。两种都试，谁成谁上，并把结论打进日志：比读代码猜硬，
     * 而且这一行日志就是 M3 那份「首帧形状」证据的数据 track 版本。 */
    size_t skip = 0;
    nlohmann::json j;
    /* 解析成功那一次用的**原文**（已剥掉容器前缀）。发布端角色要把这份逐字节
     * 发回给订阅方，见下面 catalog_json_raw_ 的说明与 SetCatalogJson。 */
    std::string raw_json;

    for (int attempt = 0; attempt < 2; attempt++) {
        const uint8_t *p = payload;
        size_t n = len;
        if (attempt == 1) {
            size_t off = 0;
            if (!MlSkipVarint(payload, len, off)) break;
            p += off;
            n -= off;
            skip = off;
        }
        const std::string body(reinterpret_cast<const char *>(p), n);
        j = nlohmann::json::parse(body, nullptr, false);
        if (!j.is_discarded()) {
            raw_json = body;
            break;
        }
    }

    if (j.is_discarded() || !j.is_object()) {
        /* 解不出来就把头部原样打出来 —— 静默丢弃会让 M2 看起来"什么都没发生" */
        std::string head;
        const size_t n = len < 48 ? len : 48;
        char b[4];
        for (size_t i = 0; i < n; i++) {
            snprintf(b, sizeof(b), "%02x", payload[i]);
            head += b;
        }
        LOG_WARN("[moq-lite] %s:%d catalog frame is not JSON (%zu bytes, "
                 "container prefix skip=%zu, head=%s)",
                 remote_ip_.c_str(), remote_port_, len, skip, head.c_str());
        return;
    }

    LOG_INFO("[moq-lite] %s:%d catalog: %zu bytes, container prefix skip=%zu, "
             "parsed ok", remote_ip_.c_str(), remote_port_, len, skip);

    /* 原文留一份。发布端角色（moq-lite export）要把它**逐字节**回给订阅方，
     * 而 codec 串（avc1.PPCCLL / mp4a.40.2）与 clock 节从 FLV 序列头反推不出来。
     * ⚠️ 存原文而不是 j.dump()：dump 会重排键序、改写数字写法，而 export flv
     * 读的就是这份，逐字节相同最省事。 */
    catalog_json_raw_ = raw_json;
    PushCatalogToManager();

    /* clock 节：wall = PTS 0 的 epoch 毫秒（moq epoch 起算），timescale 通常
     * 1000000。M3 拼绝对时间轴要用。catalog 里**没有** track timescale ——
     * 那在每条 track 的 TRACK_INFO 里，-06 的 FRAME 头带的就是它的单位。 */
    auto cit = j.find("clock");
    if (cit != j.end() && cit->is_object()) {
        clock_wall_ = cit->value("wall", static_cast<uint64_t>(0));
        clock_timescale_ = cit->value("timescale", static_cast<uint64_t>(0));
        LOG_INFO("[moq-lite] %s:%d catalog clock: wall=%llu timescale=%llu",
                 remote_ip_.c_str(), remote_port_,
                 static_cast<unsigned long long>(clock_wall_),
                 static_cast<unsigned long long>(clock_timescale_));
    }

    /* 只认 video / audio 两节。其余（text / json / binary / 以及 import ts 的
     * mpegts 扩展）本轮不管 —— 参考实现的容错规则是「未知字段一律忽略」，
     * 我们也一样：catalog 里多什么节都不该让这里失败。 */
    static const char *const kKinds[] = {"video", "audio"};
    for (size_t k = 0; k < sizeof(kKinds) / sizeof(kKinds[0]); k++) {
        const char *kind = kKinds[k];
        /* 逐节显式给优先级，不按数组下标推 —— 下标与「哪个是音频」的耦合
         * 是那种改动一次顺序就静默错掉的东西。 */
        const uint64_t prio = (std::string(kind) == "audio")
                                  ? kSubPriorityAudio : kSubPriorityVideo;
        auto sit = j.find(kind);
        if (sit == j.end() || !sit->is_object()) continue;
        auto rit = sit->find("renditions");
        if (rit == sit->end() || !rit->is_object()) continue;

        for (auto it = rit->begin(); it != rit->end(); ++it) {
            if (!it.value().is_object()) continue;
            const std::string name = it.key();   /* rendition key == track 名 */

            MoqLiteTrack &ti = tracks_[name];
            const bool first_seen = ti.name.empty();
            ti.name = name;
            ti.kind = kind;
            ti.codec = it.value().value("codec", std::string());
            ti.width = it.value().value("codedWidth", static_cast<uint32_t>(0));
            ti.height = it.value().value("codedHeight", static_cast<uint32_t>(0));
            ti.sample_rate = it.value().value("sampleRate", static_cast<uint32_t>(0));
            ti.channels = it.value().value("numberOfChannels",
                                           static_cast<uint32_t>(0));

            /* description 是**十六进制**（小写、无 0x、无分隔符）的 avcC /
             * AudioSpecificConfig 原字节 —— M3 靠它拼 FLV 序列头。⚠️ 隔壁
             * cmaf 容器的 init 字段是 base64，两者别再搞混。 */
            auto dit = it.value().find("description");
            if (dit != it.value().end() && dit->is_string()) {
                std::vector<uint8_t> cfg;
                if (HexDecode(dit->get<std::string>(), &cfg)) {
                    ti.codec_config = cfg;
                } else {
                    LOG_WARN("[moq-lite] %s:%d track \"%s\": description is not hex",
                             remote_ip_.c_str(), remote_port_, name.c_str());
                }
            }

            /* M3 只落地 H.264 与 AAC —— 这两条能拼出 FLV 序列头（avcC /
             * AudioSpecificConfig 原字节），也正是 http-flv 与浏览器的
             * moq 拉流路径都认的形状。别的编解码器（av01 / hvc1 / opus…）
             * 订阅照订、计数照记，但不往 MediaStreamManager 送，
             * 免得下游拿到一条它解不开的轨。 */
            ti.m3_usable = (std::string(kind) == "video" &&
                            ti.codec.compare(0, 5, "avc1.") == 0) ||
                           (std::string(kind) == "audio" &&
                            ti.codec.compare(0, 8, "mp4a.40.") == 0);

            if (first_seen) {
                LOG_INFO("[moq-lite] %s:%d catalog track \"%s\": kind=%s "
                         "codec=\"%s\" config=%zu bytes %ux%u %uHz %uch m3=%s",
                         remote_ip_.c_str(), remote_port_, name.c_str(), kind,
                         ti.codec.c_str(), ti.codec_config.size(),
                         ti.width, ti.height, ti.sample_rate, ti.channels,
                         ti.m3_usable ? "yes" : "no");
                /* 序列头是从 codec_config 拼的，而 catalog 的 description 是
                 * **hex**（avcC / AudioSpecificConfig 原字节）。先把它的开头
                 * 打出来：形状不对的话（比如被 base64 或加了 FLV tag 头）
                 * 这里一眼可见，不必等拉流端花屏再回头查。
                 * ⚠️ 隔壁 cmaf 容器的 init 字段是 base64，两者别再搞混。 */
                if (ti.m3_usable && !ti.codec_config.empty()) {
                    std::string cfg_hex;
                    const size_t n = ti.codec_config.size() < 16
                                         ? ti.codec_config.size() : 16;
                    char hb[4];
                    for (size_t i = 0; i < n; i++) {
                        snprintf(hb, sizeof(hb), "%02x", ti.codec_config[i]);
                        cfg_hex += hb;
                    }
                    LOG_INFO("[moq-lite] %s:%d track \"%s\" codec_config[%zu] "
                             "head=%s%s",
                             remote_ip_.c_str(), remote_port_, name.c_str(),
                             ti.codec_config.size(), cfg_hex.c_str(),
                             ti.codec_config.size() > n ? " ..." : "");
                }
            }

            /* 订阅每条媒体 track。subscribe_id == 0 表示还没订 —— catalog 每次
             * 更新都会重发整份，这里不能重复订阅。 */
            if (ti.subscribe_id == 0) {
                Subscribe(broadcast_, name, prio);
            }
        }
    }
}

void MoqLiteSession::OnMediaFrame(uint64_t sid, RecvStream &st,
                                  const uint8_t *payload, size_t len) {
    auto sit = subs_.find(st.sub_id_in_group);
    if (sit == subs_.end()) return;   /* 没订阅过的 id，上层已经 reset 了 */
    const std::string &name = sit->second;

    if (name == kCatalogTrack) {
        OnCatalogFrame(payload, len);
        return;
    }

    auto tit = tracks_.find(name);
    if (tit == tracks_.end()) return;
    MoqLiteTrack &ti = tit->second;
    ti.frames++;

    /* ── ① 剥容器时间戳前缀 ──
     * payload = [绝对微秒 varint][编解码载荷]。**这一层不能省**：省了的话
     * 媒体层收到的是一个以微秒整数开头的"NALU"，现象是花屏/解析失败而不是
     * 报错（结论 11，本轮已实测：视频 80 99 88 b0 = 10062000 = 外层 ts）。
     * 单位是**微秒**，与 track timescale 无关（hang 的容器 TIMESCALE=MICRO）。 */
    if (len == 0) {
        /* 连时间戳都没有的退化帧。参考实现解不出来（decode 空 buffer 直接失败），
         * 所以线上不该有 —— 记 DEBUG 不当错误，也不占关键帧状态机。 */
        LOG_DEBUG("[moq-lite] %s:%d track \"%s\" group=%llu empty frame",
                  remote_ip_.c_str(), remote_port_, name.c_str(),
                  static_cast<unsigned long long>(st.group));
        return;
    }

    size_t off = 0;
    uint64_t inner_us = 0;
    if (!MlReadVarint(payload, len, off, inner_us)) {
        if (!ti.prefix_bad_warned) {
            ti.prefix_bad_warned = true;
            LOG_WARN("[moq-lite] %s:%d track \"%s\": frame carries no container "
                     "timestamp prefix (len=%zu), dropping",
                     remote_ip_.c_str(), remote_port_, name.c_str(), len);
        }
        return;
    }

    /* ── ② 剥完前缀就空了 = 组结束哨兵 ──
     * 这才是它的**真实线上形状**（上一次实测踩到的坑：我原先只认 len==0，
     * 而哨兵其实是「4 字节 varint 前缀 + 0 字节载荷」，于是掉进了「太短」那个
     * 错误分支、被 WARN + drop 掉了）。
     * 依据 rs/moq-mux/src/container/legacy/mod.rs：
     *   fn end() = frame.payload.is_empty().then_some(frame.timestamp)
     * 即**解出时间戳之后**载荷为空才算哨兵；`finish_group` 只给 Video 轨写它，
     * 且写的是组的**结束时刻**，不是一帧媒体。既不送解码、也不碰关键帧
     * 状态机（视频组开头的哨兵若存在，key_pending 必须活下来 —— 这里 return
     * 得早，正好不碰）。 */
    if (off >= len) {
        LOG_DEBUG("[moq-lite] %s:%d track \"%s\" group=%llu end-of-group marker, "
                  "ends at %llu us",
                  remote_ip_.c_str(), remote_port_, name.c_str(),
                  static_cast<unsigned long long>(st.group),
                  static_cast<unsigned long long>(inner_us));
        return;
    }
    const uint8_t *data = payload + off;
    const size_t data_len = len - off;

    /* 🔎 内层微秒 vs 外层 zigzag 累加：两者**本应相等**（实测相等，
     * 因为 track timescale 就是 1e6）。不等就说明 timescale 不是 1e6 ——
     * 那时外层的数我们不能解释，但内层依然自描述，所以以**内层为准**，
     * 把这件事打出来就好。这是最省事的一次 timescale 探针：不必去订阅
     * TRACK_INFO（结论 15 正是为省掉那类流而跳过的）。 */
    if (!ti.ts_mismatch_warned &&
        static_cast<uint64_t>(st.ts_acc) != inner_us) {
        ti.ts_mismatch_warned = true;
        LOG_WARN("[moq-lite] %s:%d track \"%s\": outer ts=%lld (track "
                 "timescale) != inner %llu us - using the inner (us) one; "
                 "implied timescale ratio=%lld",
                 remote_ip_.c_str(), remote_port_, name.c_str(),
                 static_cast<long long>(st.ts_acc),
                 static_cast<unsigned long long>(inner_us),
                 static_cast<long long>(inner_us ? st.ts_acc / static_cast<int64_t>(inner_us) : 0));
    }

    /* ── ③ 首帧形状校验 ──
     * 剥掉前缀之后视频应当是**长度前缀 NALU**，音频是裸 AAC。
     * ⚠️ 长度前缀的宽度**不是恒 4**：它由 avcC 第 5 字节的低 2 位声明
     * （lengthSizeMinusOne）。上一版守卫写死「前 3 字节为 0」，被实测打脸 ——
     * 视频首帧是 `00 00 02 72 06 05 …`，第 3 字节是 0x02，是**合法的 4 字节
     * 长度前缀**（长度 0x272 = 626），守卫却报了 unexpected shape。
     * 现在按 avcC 声明的宽度去读长度并与实际载荷对上，这才是真校验。 */
    size_t nal_len_size = 4;
    if (ti.codec_config.size() >= 5) {
        nal_len_size = static_cast<size_t>(ti.codec_config[4] & 0x03) + 1;
    }
    const bool video = (ti.kind == "video");

    if (!ti.shape_logged) {
        ti.shape_logged = true;
        std::string head;
        const size_t n = data_len < 16 ? data_len : 16;
        char b[4];
        for (size_t i = 0; i < n; i++) {
            snprintf(b, sizeof(b), "%02x", data[i]);
            head += b;
        }

        bool shape_ok = true;
        uint32_t nal_len = 0;
        uint8_t nal_type = 0;
        /* NAL 类型：1 非 IDR 片 / 5 IDR 片 / 6 SEI / 7 SPS / 8 PPS。
         * 首帧是 5 还是 6 开头，直接告诉我们关键帧标志判得对不对。 */
        char nal_info[48] = "";
        if (video) {
            shape_ok = (data_len >= nal_len_size);
            if (shape_ok) {
                for (size_t i = 0; i < nal_len_size; i++) {
                    nal_len = (nal_len << 8) | data[i];
                }
                /* 长度必须落在载荷内，且第一个 NALU 的头字节得读得到。
                 * 用 <= 而不是 ==：一帧可以装多个 NALU，我们只校验头一个。 */
                shape_ok = (nal_len > 0) &&
                           (nal_len_size + static_cast<size_t>(nal_len) <= data_len);
                if (shape_ok) {
                    nal_type = static_cast<uint8_t>(data[nal_len_size] & 0x1f);
                }
            }
            snprintf(nal_info, sizeof(nal_info), "nal_len_size=%zu nal_len=%u nal_type=%u ",
                     nal_len_size, nal_len, static_cast<unsigned>(nal_type));
        }
        LOG_INFO("[moq-lite] %s:%d track \"%s\" first frame: us=%llu ts_ms=%lld "
                 "len=%zu (payload %zu) %shead=%s",
                 remote_ip_.c_str(), remote_port_, name.c_str(),
                 static_cast<unsigned long long>(inner_us),
                 static_cast<long long>(inner_us / static_cast<uint64_t>(kUsPerMs)),
                 len, data_len, nal_info, head.c_str());
        if (!shape_ok) {
            LOG_WARN("[moq-lite] %s:%d track \"%s\": payload is not a "
                     "length-prefixed NALU after stripping the container prefix "
                     "(nal_len_size=%zu, declared nal_len=%u, payload=%zu); the "
                     "container may carry more bytes than expected",
                     remote_ip_.c_str(), remote_port_, name.c_str(),
                     nal_len_size, nal_len, data_len);
        }
    }

    if (!ti.m3_usable) return;   /* codec 不认识，已订阅但不落地 */

    /* ── ④ 送进 MediaStreamManager ──
     * 时间轴用**内层绝对微秒**，媒体层要毫秒。
     * cts 恒 0：Legacy 容器只带一个「显示时间戳」（hang::container::Frame
     * 就是 {timestamp, payload}），**没有 composition time 字段** ——
     * 所以这里无法还原 pts - dts，参考实现自己也还原不了。 */
    const int64_t ts_ms = static_cast<int64_t>(inner_us / static_cast<uint64_t>(kUsPerMs));
    /* key 只对视频有意义：FLV 的音频 tag 不看关键帧位（EmitFlvAudioFrame 忽略
     * 它），而音频一帧一组、frame_start 恒 0，key_pending 会一直是 true ——
     * 上一版就因此在日志里把每个音频帧都打成 key=1。 */
    const bool key = video && st.key_pending;
    st.key_pending = false;   /* 关键帧只在组内第一帧成立 */

    /* 写盘之前先认领（同名流新顶旧）。放在这里而不是构造时：broadcast 要等
     * announce、track 要等 catalog，而**没有媒体帧就不该占坑** ——
     * 否则一条只订阅了 catalog 的会话会把流名占住不给真 publisher。 */
    if (!EnsureStreamClaimed()) return;

    if (ti.kind == "video") {
        if (!ti.seq_hdr_sent) {
            ti.seq_hdr_sent = true;
            EmitFlvVideoSeqHdr(ti, ts_ms);
        }
        EmitFlvVideoFrame(ti, ts_ms, /*seq=*/false, key, data, data_len);
    } else {
        if (!ti.seq_hdr_sent) {
            ti.seq_hdr_sent = true;
            EmitFlvAudioSeqHdr(ti, ts_ms);
        }
        EmitFlvAudioFrame(ti, ts_ms, /*seq=*/false, data, data_len);
    }

    if (ti.delivered <= kDeliverLogLimit) {
        LOG_DEBUG("[moq-lite] %s:%d track \"%s\" -> media: group=%llu frame=%llu "
                  "ts_ms=%lld bytes=%zu key=%d",
                  remote_ip_.c_str(), remote_port_, name.c_str(),
                  static_cast<unsigned long long>(st.group),
                  static_cast<unsigned long long>(st.frame_index),
                  static_cast<long long>(ts_ms), data_len, key ? 1 : 0);
    }

    /* 落地的计数走 **INFO**，而且是每 kDeliverLogEvery 帧一条。
     * 理由（上一轮实测的教训）：原来只有那条带 <=3 上限的 DEBUG 行，于是
     * 「送进去几帧」在日志里根本读不出来 —— 看到的 6 行是上限不是总数，
     * 而 INFO 级别下更是一条都没有，验收只能干等会话关闭时的收尾摘要。
     * 每 100 帧一条：25fps 的视频约 4 秒一行，47fps 的音频约 2 秒一行，
     * 量级可以忽略，却让 INFO 级别就能验「帧真的进了 MediaStreamManager」。 */
    if (ti.delivered == 1) {
        LOG_INFO("[moq-lite] %s:%d track \"%s\" delivery started: first frame is "
                 "in MediaStreamManager (stream=\"%s\")",
                 remote_ip_.c_str(), remote_port_, name.c_str(),
                 stream_key_.c_str());
    } else if (ti.delivered % kDeliverLogEvery == 0) {
        LOG_INFO("[moq-lite] %s:%d track \"%s\" delivered=%llu frames "
                 "(last ts_ms=%lld)",
                 remote_ip_.c_str(), remote_port_, name.c_str(),
                 static_cast<unsigned long long>(ti.delivered),
                 static_cast<long long>(ts_ms));
    }
    (void)sid;
}

/* ============================================================
 * M3：媒体落地 —— MediaStreamManager
 * ============================================================ */

void MoqLiteSession::BuildStreamKey() {
    if (broadcast_.empty() || !stream_key_.empty()) return;

    /* app 段 = 服务端入口路径（客户端拨的 URL 最后一段）。发布端的 broadcast
     * 命名空间根在 ""（实测：ANNOUNCE prefix 发 "" 才有回音），它自己不带
     * app 概念，所以这一段只能由服务端定 —— 而它本来就是配置项，
     * moqlite_subpath 里写 live 就得到 live/xxx。 */
    std::string app = routed_path_;
    if (app.empty()) {
        app = kFallbackApp;
        LOG_WARN("[moq-lite] %s:%d no routed path; using \"%s\" as the stream "
                 "key app segment for broadcast \"%s\"",
                 remote_ip_.c_str(), remote_port_, kFallbackApp,
                 broadcast_.c_str());
    }

    stream_key_ = app + "/" + broadcast_;
    /* 与 MediaStreamManager::GetAppStreamname 同一套切法（第一个 '/' 处切），
     * 保证包上的 app_/streamname_ 和 key_ 三者自洽。 */
    const size_t pos = stream_key_.find('/');
    stream_app_ = stream_key_.substr(0, pos);
    stream_name_ = stream_key_.substr(pos + 1);

    LOG_INFO("[moq-lite] %s:%d media stream key=\"%s\" (app=\"%s\" stream=\"%s\") "
             "owner=%s",
             remote_ip_.c_str(), remote_port_, stream_key_.c_str(),
             stream_app_.c_str(), stream_name_.c_str(), owner_id_.c_str());
}

bool MoqLiteSession::EnsureStreamClaimed() {
    if (stream_claimed_) return true;
    /* 会话已经在关闭途中（可能是上面 kRejected 那次 CloseSession 刚把牌子摘了）。
     * 必须挡住：QuicConnectionClose 到真正回调之间有 **1 秒** 的 CLOSING 窗口
     * （quic_connection.c 的 close_timer），这期间对端的 GROUP 还在进来 ——
     * 不挡就会重新认领一遍牌子、继续往一条已经关掉的会话里灌媒体。 */
    if (dead_) return false;
    if (stream_key_.empty()) {
        /* broadcast 还没定下来就来了媒体帧 —— 不该发生（订阅要等 catalog，
         * catalog 要等 broadcast），真出现就是协议层错乱，记一笔。 */
        LOG_WARN("[moq-lite] %s:%d media frame before the stream key was known",
                 remote_ip_.c_str(), remote_port_);
        return false;
    }
    stream_claimed_ = true;

    const PublishClaimResult claim =
        MediaStreamManager::ClaimPublisher(stream_key_, owner_id_);
    switch (claim.action) {
    case PublishClaim::kTakeOver:
        /* 同名流同时只能有一个源（draft-ietf-moq-transport §2.4.3）。
         * 旧会话从这一刻起 IsPublisherOwner 返回 false，它自己的每一帧都会
         * 在 Deliver 里被丢掉 —— 所以不会出现两条时间线交织。
         * ⚠️ 但我们**没有**去断开旧会话：moq-lite 侧还没有
         * owner_id → session 的登记表（draft-20 那条线有，见
         * MoqRfcHandler::KickPublisherSession）。旧连接会挂在 QUIC idle
         * 超时上自然消失，期间只是白收包，不影响画面。 */
        LOG_WARN("[moq-lite] %s:%d take over stream \"%s\": old owner=%s new=%s "
                 "(the old session stops feeding via the owner check; its "
                 "connection is left to idle out)",
                 remote_ip_.c_str(), remote_port_, stream_key_.c_str(),
                 claim.previous_owner.c_str(), owner_id_.c_str());
        break;
    case PublishClaim::kRejected:
        /* 顶替太频繁（两端撞名互踢）：保留在位的那个，断开新来的这个。 */
        LOG_ERROR("[moq-lite] %s:%d claim rejected for stream \"%s\" "
                  "(keep=%s new=%s, too many takeovers)",
                  remote_ip_.c_str(), remote_port_, stream_key_.c_str(),
                  claim.previous_owner.c_str(), owner_id_.c_str());
        CloseSession(kSessionInternalError, "publisher claim rejected");
        return false;
    case PublishClaim::kReused:
        LOG_INFO("[moq-lite] %s:%d stream \"%s\" reused (owner=%s)",
                 remote_ip_.c_str(), remote_port_, stream_key_.c_str(),
                 owner_id_.c_str());
        break;
    default:
        LOG_INFO("[moq-lite] %s:%d stream \"%s\" created (owner=%s)",
                 remote_ip_.c_str(), remote_port_, stream_key_.c_str(),
                 owner_id_.c_str());
        break;
    }

    /* 认领成功后才好把 catalog 推进去：catalog 常常比第一个媒体包先到，那时
     * MediaStream 还没被建出来（由 AddPublisher 在第一个媒体包上创建），
     * 早推会找不到目标。反过来（媒体先到）在 OnCatalogFrame 里已经推过一次，
     * 这里是空操作。 */
    PushCatalogToManager();
    return true;
}

void MoqLiteSession::PushCatalogToManager() {
    /* 两个前提缺一不可：有 catalog 原文、且这条流已经认领（MediaStream 存在）。
     * 幂等 —— SetCatalogJson 内容相同就什么都不做。 */
    if (catalog_json_raw_.empty() || !stream_claimed_ || stream_key_.empty()) return;
    MediaStreamManager::SetCatalogJson(stream_key_, owner_id_, catalog_json_raw_);
}

bool MoqLiteSession::IsStreamOwner() {
    if (stream_key_.empty() || owner_id_.empty()) return false;
    return MediaStreamManager::IsPublisherOwner(stream_key_, owner_id_);
}

void MoqLiteSession::ReleaseStream() {
    if (!stream_claimed_) return;
    stream_claimed_ = false;
    if (stream_key_.empty()) return;
    /* 只有 owner 能摘牌。被顶掉的旧会话也会走到这里，MediaStreamManager 会
     * 拒绝它 —— 那正是我们要的：别把新 publisher 刚建立的 cache 清掉。 */
    if (MediaStreamManager::ReleasePublisher(stream_key_, owner_id_)) {
        LOG_INFO("[moq-lite] %s:%d released stream \"%s\" (owner=%s)",
                 remote_ip_.c_str(), remote_port_, stream_key_.c_str(),
                 owner_id_.c_str());
    } else {
        LOG_INFO("[moq-lite] %s:%d stream \"%s\" not released (owner=%s is no "
                 "longer the publisher)",
                 remote_ip_.c_str(), remote_port_, stream_key_.c_str(),
                 owner_id_.c_str());
    }
}

bool MoqLiteSession::Deliver(const char *what, Media_Packet_Ptr pkt) {
    /* owner 校验必须在我们这边 —— WriterMediaPacket 自己不做（它的注释就写着
     * 媒体路径自己守）。被顶掉的会话不该再把帧灌进同一条流。 */
    if (!IsStreamOwner()) {
        const int64_t now_s = now_millisec() / 1000;
        if (owner_drop_warn_s_ != now_s) {
            owner_drop_warn_s_ = now_s;
            LOG_WARN("[moq-lite] %s:%d dropping %s for \"%s\": superseded by "
                     "another publisher (owner=%s)",
                     remote_ip_.c_str(), remote_port_, what,
                     stream_key_.c_str(), owner_id_.c_str());
        }
        return false;
    }
    MediaStreamManager::WriterMediaPacket(pkt);
    return true;
}

/* FLV tag 头照抄 moq_push.cpp 的 EmitFlvVideo / EmitFlvAudio（那两个是
 * MoqPushSession 的私有成员，拿不过来，只能复刻形状）。
 * 字节布局与参考实现的导出侧**逐字节一致**：
 *   rs/moq-mux/src/container/flv/export.rs 的 video_sequence_header
 *     = [(KEY<<4)|AVC][AVC_SEQUENCE_HEADER][0,0,0][description]
 *   audio_sequence_header
 *     = [AAC_AUDIO_TAG_HEADER][AAC_SEQUENCE_HEADER][description]
 * 所以 catalog 的 description 原字节直接写进去就是 FLV 序列头。 */

void MoqLiteSession::EmitFlvVideoSeqHdr(MoqLiteTrack &ti, int64_t ts_ms) {
    if (ti.codec_config.empty()) {
        LOG_WARN("[moq-lite] %s:%d track \"%s\": no codec description, cannot "
                 "build the FLV video sequence header (late joiners will not "
                 "be able to decode)",
                 remote_ip_.c_str(), remote_port_, ti.name.c_str());
        return;
    }
    EmitFlvVideoFrame(ti, ts_ms, /*seq=*/true, /*key=*/true,
                      ti.codec_config.data(), ti.codec_config.size());
}

void MoqLiteSession::EmitFlvAudioSeqHdr(MoqLiteTrack &ti, int64_t ts_ms) {
    if (ti.codec_config.empty()) {
        LOG_WARN("[moq-lite] %s:%d track \"%s\": no codec description, cannot "
                 "build the FLV audio sequence header",
                 remote_ip_.c_str(), remote_port_, ti.name.c_str());
        return;
    }
    EmitFlvAudioFrame(ti, ts_ms, /*seq=*/true, ti.codec_config.data(),
                      ti.codec_config.size());
}

void MoqLiteSession::EmitFlvVideoFrame(MoqLiteTrack &ti, int64_t ts_ms,
                                       bool seq, bool key, const uint8_t *data,
                                       size_t len) {
    const size_t body_len = 5 + len;
    auto pkt = std::make_shared<Media_Packet>(body_len);
    pkt->av_type_ = MEDIA_VIDEO_TYPE;
    pkt->codec_type_ = MEDIA_CODEC_H264;
    pkt->fmt_type_ = MEDIA_FORMAT_FLV;
    pkt->dts_ = ts_ms;
    pkt->pts_ = ts_ms;          /* cts 恒 0，见 OnMediaFrame 的说明 */
    pkt->is_seq_hdr_ = seq;
    pkt->is_key_frame_ = !seq && key;
    pkt->app_ = stream_app_;
    pkt->streamname_ = stream_name_;
    pkt->key_ = stream_key_;
    /* CompositionTime 24 位有符号，我们没有这个信息，写 0。 */
    const uint8_t hdr[5] = {
        static_cast<uint8_t>((seq || key) ? (FLV_VIDEO_KEY_FLAG | FLV_VIDEO_H264_CODEC)
                                          : (FLV_VIDEO_INTER_FLAG | FLV_VIDEO_H264_CODEC)),
        static_cast<uint8_t>(seq ? FLV_VIDEO_AVC_SEQHDR : FLV_VIDEO_AVC_NALU),
        0, 0, 0,
    };
    pkt->buffer_ptr_->AppendData(reinterpret_cast<const char *>(hdr), 5);
    pkt->buffer_ptr_->AppendData(reinterpret_cast<const char *>(data), len);

    if (!Deliver(seq ? "video seq hdr" : "video", pkt)) return;
    if (!seq) ti.delivered++;
}

void MoqLiteSession::EmitFlvAudioFrame(MoqLiteTrack &ti, int64_t ts_ms,
                                       bool seq, const uint8_t *data,
                                       size_t len) {
    const size_t body_len = 2 + len;
    auto pkt = std::make_shared<Media_Packet>(body_len);
    pkt->av_type_ = MEDIA_AUDIO_TYPE;
    pkt->codec_type_ = MEDIA_CODEC_AAC;
    pkt->fmt_type_ = MEDIA_FORMAT_FLV;
    pkt->dts_ = ts_ms;
    pkt->pts_ = ts_ms;
    pkt->is_seq_hdr_ = seq;
    pkt->is_key_frame_ = seq;
    pkt->app_ = stream_app_;
    pkt->streamname_ = stream_name_;
    pkt->key_ = stream_key_;
    const uint8_t hdr[2] = {
        static_cast<uint8_t>(FLV_AUDIO_AAC_CODEC | 0x0f),
        static_cast<uint8_t>(seq ? 0x00 : 0x01),
    };
    pkt->buffer_ptr_->AppendData(reinterpret_cast<const char *>(hdr), 2);
    pkt->buffer_ptr_->AppendData(reinterpret_cast<const char *>(data), len);

    if (!Deliver(seq ? "audio seq hdr" : "audio", pkt)) return;
    if (!seq) ti.delivered++;
}

/* ============================================================
 * GROUP（uni 0x0）—— 媒体与 catalog 都从这里来
 * ============================================================ */

void MoqLiteSession::ParseGroupStream(uint64_t sid, RecvStream &st) {
    if (st.done) {
        st.buf.clear();
        return;
    }

    /* GROUP{ Message Length(i); Subscribe ID(i); Group Sequence(i); Frame Start(i) }
     * ⚠️ Frame Start 是 06 新增的（05 只有前三项）。少读这一个 varint，后面
     * 第一个 FRAME 的时间戳差就整个解错 —— 而且**不会报错**，只会解出一堆
     * 看似合理但没有意义的数字。 */
    if (!st.group_hdr_done) {
        size_t off = 0, blen = 0;
        const MlMsgStatus s = MlReadMessage(st.buf.data(), st.buf.size(), off, blen);
        if (s == kMlMsgTooLarge) {
            ResetStream(sid, kStreamInternalError, st, "group header over max size");
            return;
        }
        if (s == kMlMsgNeedMore) {
            if (st.fin) {
                LOG_WARN("[moq-lite] %s:%d group stream %llu ended inside its header",
                         remote_ip_.c_str(), remote_port_,
                         static_cast<unsigned long long>(sid));
                st.done = true;
                st.buf.clear();
            }
            return;
        }

        const size_t end = off + blen;
        const uint8_t *p = st.buf.data();
        size_t pos = off;
        uint64_t sub_id = 0, seq = 0, frame_start = 0;
        if (!MlReadVarint(p, end, pos, sub_id) ||
            !MlReadVarint(p, end, pos, seq) ||
            !MlReadVarint(p, end, pos, frame_start) || pos != end) {
            ResetStream(sid, kStreamInternalError, st, "malformed group header");
            return;
        }

        auto sit = subs_.find(sub_id);
        if (sit == subs_.end()) {
            /* 没订阅过的 Subscribe ID。参考实现按「已取消」处理：放弃这条流，
             * **不**升级成会话错误（rs/lite/subscriber.rs:765-783）。 */
            ResetStream(sid, kStreamCancelled, st, "group for an unknown subscribe id");
            return;
        }

        st.group_hdr_done = true;
        st.sub_id_in_group = sub_id;
        st.group = seq;
        /* 06 语义：这条流上的第一个帧就是组内第 frame_start 帧。于是
         * 「frame_index == 0」这个判据天然等于「这条流的第一个帧」，
         * 而 frame_start > 0 时它本来就不是关键帧 —— 正好对上。 */
        st.frame_index = frame_start;
        /* 关键帧判据：Legacy 线上**没有 keyframe 标志位**，一组里第一个
         * 非哨兵帧就是关键帧。frame_start > 0 表示我们从组中间开始收，
         * 那第一帧不是关键帧。用标志位而不是比 frame_index —— 空载荷哨兵
         * 也占 frame_index，用它判会把哨兵当成关键帧。 */
        st.key_pending = (frame_start == 0);
        /* 时间戳差是**组内**累加：每组的第一个帧相对 0 取差，所以基线按组重置。
         * frame_start > 0 时前几帧的差我们没收到，这个累加值会偏小 —— 记一笔，
         * 真出现时（订阅中途才开始）M3 需要按 SUBSCRIBE_START 的 group 对齐。 */
        st.ts_acc = 0;
        st.buf.erase(st.buf.begin(), st.buf.begin() + static_cast<std::ptrdiff_t>(end));

        if (sit->second != kCatalogTrack) {
            auto tit = tracks_.find(sit->second);
            if (tit != tracks_.end()) tit->second.groups++;
        }
        LOG_DEBUG("[moq-lite] %s:%d GROUP: stream=%llu sub=%llu track=\"%s\" "
                  "group=%llu frame_start=%llu",
                  remote_ip_.c_str(), remote_port_,
                  static_cast<unsigned long long>(sid),
                  static_cast<unsigned long long>(sub_id), sit->second.c_str(),
                  static_cast<unsigned long long>(seq),
                  static_cast<unsigned long long>(frame_start));
    }

    /* 之后是这个组的 FRAME 序列 —— **裸消息，没有类型字节**：
     *   FRAME{ Timestamp Delta(i, zigzag); Message Length(i); Payload }
     * 时间戳差是 track timescale 单位的有符号值，zigzag 编码。 */
    for (;;) {
        size_t off = 0;
        uint64_t zz = 0;
        if (!MlReadVarint(st.buf.data(), st.buf.size(), off, zz)) break;
        size_t boff = off, blen = 0;
        const MlMsgStatus s = MlReadMessage(st.buf.data(), st.buf.size(), boff, blen);
        if (s == kMlMsgTooLarge) {
            ResetStream(sid, kStreamInternalError, st, "frame over max message size");
            return;
        }
        if (s == kMlMsgNeedMore) break;

        const size_t end = boff + blen;
        st.ts_acc += MlZigzagDecode(zz);
        /* ⚠️ 先把帧交出去，再 erase —— OnMediaFrame 拿到的指针指向 st.buf 内部，
         * erase 之后就是野的。OnMediaFrame 里可能开新流（订阅），但 std::map
         * 的节点是稳定的，st 这个引用不会被它弄失效。 */
        OnMediaFrame(sid, st, st.buf.data() + boff, static_cast<size_t>(blen));
        st.buf.erase(st.buf.begin(), st.buf.begin() + static_cast<std::ptrdiff_t>(end));
        st.frame_index++;
    }

    if (st.fin) {
        /* 流的 FIN 落在帧边界上是**正常收尾**（这个组到头了）。落在帧中间才是
         * 截断 —— 那时 reset 也没用了（对端已经发完了），记一笔就走。 */
        if (!st.buf.empty()) {
            LOG_WARN("[moq-lite] %s:%d group stream %llu ended mid-frame, "
                     "%zu trailing bytes dropped",
                     remote_ip_.c_str(), remote_port_,
                     static_cast<unsigned long long>(sid), st.buf.size());
        } else {
            LOG_DEBUG("[moq-lite] %s:%d group stream %llu done: sub=%llu group=%llu "
                      "frames=%llu", remote_ip_.c_str(), remote_port_,
                      static_cast<unsigned long long>(sid),
                      static_cast<unsigned long long>(st.sub_id_in_group),
                      static_cast<unsigned long long>(st.group),
                      static_cast<unsigned long long>(st.frame_index));
        }
        st.done = true;
        st.buf.clear();
    }
}

/* ============================================================
 * M4：发布端角色 —— 对端开的 announce / subscribe / track 流
 *
 * 三条流都遵循同一套骨架：
 *   ① 一次性消息（Track）或首条消息（Announce/Subscribe）用 MlReadMessage 切；
 *   ② 解析后立刻 erase 掉已消费的字节，**不要再碰 st**（后面的发送可能
 *      触发 QUIC 回调）；
 *   ③ 任何「不该继续」的判定一律 ResetStream 那条流，**不关会话** ——
 *      对端请求一个我们没有的 broadcast 是正常情况，不是协议违规。
 * ============================================================ */

namespace {

/* Legacy 容器下行的时间尺度。**必须是 1000000（微秒）**：payload 开头那个
 * 绝对时间戳前缀恒为微秒（hang 的 TIMESCALE=MICRO），而外层 FRAME 差值的单位
 * 就是我们在 TRACK_INFO 里声明的 track timescale —— 个尺度必须一致，
 * 否则对端按另一个尺度换算差值，表现是播放速度离谱/时间轴乱跳。 */
constexpr uint64_t kServeTimescale = 1000000;
/* TRACK_INFO 里的 publisher max age。它是**发布端自己的**缓存承诺，与订阅方
 * 请求的 max_age 无关（那个在 SUBSCRIBE 里，是订阅方的偏好）。 */
constexpr uint64_t kServeMaxAgeMs = 5000;
constexpr uint8_t  kServePriority = 0;

/* ANNOUNCE_REQUEST 的 prefix 是**绝对**路径，ANNOUNCE_START 的 suffix 是相对它
 * 的路径（publisher.rs:543-574 的 rebase）。不在 scope 内返回 false。 */
bool AnnounceSuffixOf(const std::string &wire, const std::string &prefix,
                      std::string *suffix) {
    if (prefix.empty()) {
        *suffix = wire;
        return true;
    }
    if (wire == prefix) {
        *suffix = std::string();
        return true;
    }
    if (wire.size() > prefix.size() &&
        wire.compare(0, prefix.size(), prefix) == 0 &&
        wire[prefix.size()] == '/') {
        *suffix = wire.substr(prefix.size() + 1);
        return true;
    }
    return false;
}

} /* namespace */

std::string MoqLiteSession::AnnounceWireOf(const std::string &stream_key) const {
    if (!routed_path_.empty()) {
        const std::string prefix = routed_path_ + "/";
        if (stream_key.size() > prefix.size() &&
            stream_key.compare(0, prefix.size(), prefix) == 0) {
            return stream_key.substr(prefix.size());
        }
    }
    return stream_key;
}

bool MoqLiteSession::ResolveServeTrack(const std::string &broadcast,
                                       const std::string &track,
                                       std::string *local_key,
                                       std::string *kind) {
    if (broadcast.empty() || track.empty()) return false;

    /* 本地 key 有两种命名空间，按顺序试：
     *   1) broadcast 本身 = <app>/<stream>：Edge 本地推流、Center 直接 ingest；
     *   2) <本端路由>/<broadcast>：集群上推/回源落在本端的 key。
     * 第 1 种优先：app-qualified 的广播先尝试直接命中本地 publisher；查不到
     * 再按旧规则拼路由前缀。没有 '/' 的旧式 broadcast 直接走第 2 种。 */
    const std::string app = routed_path_.empty() ? kFallbackApp : routed_path_;
    std::string key;
    std::string json;
    bool found = false;
    if (broadcast.find('/') != std::string::npos) {
        key = broadcast;
        found = MediaStreamManager::GetCatalogJson(key, json);
    }
    if (!found) {
        key = app + "/" + broadcast;
        found = MediaStreamManager::GetCatalogJson(key, json);
    }

    /* 没有 catalog 的流**不对外服务**：export flv 必须靠 catalog 才能解析出
     * rendition（codec 串 / description / clock 全在里面）。把它 announce 出去
     * 的话，对端会连上、解析出 broadcast、然后**静默什么都拿不到** —— 那是最糟
     * 的失败模式：看起来像我们的 bug，其实无处可查。RTMP 之类填不上 catalog 的
     * 来源本轮不支持导出。 */
    if (!found) return false;

    if (track == kCatalogTrack) {
        if (local_key) *local_key = key;
        if (kind) *kind = "catalog";
        return true;
    }

    /* 媒体轨：名字必须真的出现在这份 catalog 的 renditions 里。查不到就是
     * 不存在 —— 不猜、不兜底，否则对端会订上一条我们永远发不出数据的轨。
     * kind 顺便带出去：下行写入器要靠它决定过滤哪个 av_type。 */
    const nlohmann::json j = nlohmann::json::parse(json, nullptr, false);
    if (j.is_discarded() || !j.is_object()) return false;
    static const char *const kKinds[] = {"video", "audio"};
    for (size_t k = 0; k < sizeof(kKinds) / sizeof(kKinds[0]); k++) {
        auto sit = j.find(kKinds[k]);
        if (sit == j.end() || !sit->is_object()) continue;
        auto rit = sit->find("renditions");
        if (rit == sit->end() || !rit->is_object()) continue;
        if (rit->find(track) != rit->end()) {
            if (local_key) *local_key = key;
            if (kind) *kind = kKinds[k];
            return true;
        }
    }
    return false;
}

/* Track 流（bidi 0x6）—— 对端拿 track 的不可变属性，**与订阅无关**。
 *
 * ⚠️ 这条流是本轮最大的新增，也是最容易被整个跳过的一条：参考实现的订阅端在
 * 有 track 流时把状态机钉死在「先 Track、再 Subscribe」（lite/subscriber.rs
 * 的 TrackRunState::Info），并且**阻塞读** TRACK_INFO 拿 timescale，那个
 * timescale 直接决定后续所有 zigzag 时间戳差的解释。**不发 TRACK_INFO，
 * 对端一个 SUBSCRIBE 都不会发过来。** */
void MoqLiteSession::ParsePeerTrack(uint64_t sid, RecvStream &st) {
    if (st.done) {
        st.buf.clear();
        return;
    }
    if (!publisher_armed_) {
        ResetStream(sid, kStreamCancelled, st, "track request but peer is not a subscriber");
        st.done = true;
        st.buf.clear();
        return;
    }

    size_t off = 0, blen = 0;
    const MlMsgStatus s = MlReadMessage(st.buf.data(), st.buf.size(), off, blen);
    if (s == kMlMsgTooLarge) {
        ResetStream(sid, kStreamInternalError, st, "track request over max message size");
        st.done = true;
        st.buf.clear();
        return;
    }
    if (s == kMlMsgNeedMore) {
        if (st.fin) {
            LOG_WARN("[moq-lite] %s:%d track stream %llu ended inside its request",
                     remote_ip_.c_str(), remote_port_,
                     static_cast<unsigned long long>(sid));
            st.done = true;
            st.buf.clear();
        }
        return;
    }

    const size_t end = off + blen;
    const uint8_t *p = st.buf.data();
    size_t pos = off;
    std::string broadcast, track;
    if (!MlReadString(p, end, pos, broadcast) || !MlReadString(p, end, pos, track) ||
        pos != end) {
        ResetStream(sid, kStreamInternalError, st, "malformed track request");
        st.done = true;
        st.buf.clear();
        return;
    }
    st.buf.erase(st.buf.begin(), st.buf.begin() + static_cast<std::ptrdiff_t>(end));
    st.done = true;   /* Track 流是一次性的：答完就 FIN，后面不会再有消息 */

    std::string key;
    bool known = ResolveServeTrack(broadcast, track, &key, nullptr);
    if (!known) {
        /* 本地没有这条广播：可能正在/可以回源。TRACK_INFO 先按本服务端的固定
         * 参数回（timescale=1e6 是 legacy 线上的硬约定），真正的目录等拉回来
         * 再通过 SUBSCRIBE（会被挂起）兑现。不回 TRACK_INFO 的话对端**连
         * SUBSCRIBE 都不会发**，回源就永远触发不了。 */
        const std::string app = routed_path_.empty() ? kFallbackApp
                                                     : routed_path_;
        key = app + "/" + broadcast;
        /* wire = 对端订阅的 broadcast：远端广播带 app（live1/stream1），
         * 必须原样带去 Center；不能从 key 反推（key 里那段 app 是本端路由）。 */
        if (on_stream_missing_ && on_stream_missing_(key, broadcast)) {
            LOG_INFO("[moq-lite] %s:%d TRACK_INFO for remote broadcast=\"%s\" "
                     "track=\"%s\" key=\"%s\": replying with defaults, origin "
                     "pull in flight",
                     remote_ip_.c_str(), remote_port_, broadcast.c_str(),
                     track.c_str(), key.c_str());
            known = true;
        }
    }

    std::vector<uint8_t> out;
    if (!known ||
        !MlBuildTrackInfo(out, kServePriority, kServeMaxAgeMs, kServeTimescale)) {
        LOG_INFO("[moq-lite] %s:%d track request for unknown track: broadcast=\"%s\" "
                 "track=\"%s\"",
                 remote_ip_.c_str(), remote_port_, broadcast.c_str(), track.c_str());
        ResetStream(sid, kStreamCancelled, st, "unknown track");
        return;
    }

    LOG_INFO("[moq-lite] %s:%d TRACK_INFO: broadcast=\"%s\" track=\"%s\" key=\"%s\" "
             "priority=%u max_age=%llums timescale=%llu (then FIN)",
             remote_ip_.c_str(), remote_port_, broadcast.c_str(), track.c_str(),
             key.c_str(), (unsigned)kServePriority,
             (unsigned long long)kServeMaxAgeMs,
             (unsigned long long)kServeTimescale);

    /* fin=1：发布端写完 TRACK_INFO 立刻 FIN 这条流（rs/lite/publisher.rs 的
     * poll_flush + finish）。不 FIN 的话对端会一直等更多属性。 */
    if (QuicConnectionStreamSend(conn_, sid, out.data(), out.size(), 1) < 0) {
        LOG_ERROR("[moq-lite] %s:%d cannot send TRACK_INFO on stream %llu",
                  remote_ip_.c_str(), remote_port_,
                  static_cast<unsigned long long>(sid));
    }
}

/* Announce 流（bidi 0x1）—— 对端来要「你这个 scope 下有什么可拉的」。
 *
 * 应答顺序是死的：先一条 ANNOUNCE_OK（带 origin 与 active 条数），紧接
 * active 条 ANNOUNCE_START（publisher.rs 的 init 就是这个顺序）。
 * active == 0 是**合法**结果（本端确实没东西），且**必须保持流打开**继续等
 * 后续的 ANNOUNCE_START —— 对端把这里的 FIN 当成致命错误。 */
void MoqLiteSession::ParsePeerAnnounce(uint64_t sid, RecvStream &st) {
    if (st.done) {
        st.buf.clear();
        return;
    }
    if (st.stage == kPeerAnnStageWaitRequest) {
        if (!publisher_armed_) {
            ResetStream(sid, kStreamCancelled, st, "announce request but peer is not a subscriber");
            st.done = true;
            st.buf.clear();
            return;
        }

        size_t off = 0, blen = 0;
        const MlMsgStatus s = MlReadMessage(st.buf.data(), st.buf.size(), off, blen);
        if (s == kMlMsgTooLarge) {
            ResetStream(sid, kStreamInternalError, st, "announce request over max message size");
            st.done = true;
            st.buf.clear();
            return;
        }
        if (s == kMlMsgNeedMore) {
            if (st.fin) {
                LOG_WARN("[moq-lite] %s:%d announce stream %llu ended inside its request",
                         remote_ip_.c_str(), remote_port_,
                         static_cast<unsigned long long>(sid));
                st.done = true;
                st.buf.clear();
            }
            return;
        }

        const size_t end = off + blen;
        const uint8_t *p = st.buf.data();
        size_t pos = off;
        std::string prefix;
        /* ANNOUNCE_REQUEST 在 -06 上**只有前缀一个字段**：exclude_hop 是 04/05
         * 专有、hidden 是 07 专有，版本门控对 -06 都编译掉了（announce.rs:414-424）。
         * 前缀是**绝对**路径。 */
        if (!MlReadString(p, end, pos, prefix) || pos != end) {
            ResetStream(sid, kStreamInternalError, st, "malformed announce request");
            st.done = true;
            st.buf.clear();
            return;
        }
        st.buf.erase(st.buf.begin(), st.buf.begin() + static_cast<std::ptrdiff_t>(end));
        st.req_path = prefix;
        st.stage    = kPeerAnnStageRunning;
        if (!AnnounceGrid(sid, st, prefix)) return;   /* 内部已关/已 reset */
        return;
    }

    /* 阶段 2：请求已答完，流保持打开等我们推更新。对端在这条流上不会再发东西
     * （-06 的 announce 是单向的：订阅方只发那一条请求），所以剩下的字节没有
     * 合法解释。 */
    if (!st.buf.empty()) {
        ResetStream(sid, kStreamInternalError, st,
                    "unexpected bytes on announce stream after the request");
        st.done = true;
        st.buf.clear();
    }
}

/* 算一遍「请求前缀 prefix 之下我们有什么可公告的」，把 ANNOUNCE_OK + 若干条
 * ANNOUNCE_START 一次写出去。
 *
 * suffix 是**相对 prefix** 的路径（publisher.rs:543-574 的 rebase）：广播的路由
 * 恰好等于 prefix 时是空串 —— 也就是最常见的那一种。
 *
 * 日志里同时打请求前缀与算出的条数：前缀不匹配是**静默**的（参考实现对不在
 * scope 内的请求就是「回空集合 + 保持流打开」，不报错也不 reset），不把这两个
 * 数打出来，一次失败要盲查很久。 */
bool MoqLiteSession::AnnounceGrid(uint64_t sid, RecvStream &st,
                                  const std::string &prefix) {
    std::vector<std::string> wires;
    const std::vector<std::string> keys = MediaStreamManager::ListPublisherKeys();
    std::string json;
    for (size_t i = 0; i < keys.size(); i++) {
        const std::string wire = AnnounceWireOf(keys[i]);
        if (wire.empty()) continue;
        /* 没有 catalog 的流不公告（见 ResolveServeTrack 的说明） */
        if (!MediaStreamManager::GetCatalogJson(keys[i], json)) continue;
        wires.push_back(wire);
    }
    /* 集群 edge：把 Center 公告过来的广播也报给本地客户端，否则它们连
     * SUBSCRIBE 都不会发，按需回源就永远触发不了。 */
    if (announce_provider_) announce_provider_(&wires);
    std::sort(wires.begin(), wires.end());
    wires.erase(std::unique(wires.begin(), wires.end()), wires.end());

    struct Item { std::string wire; std::string suffix; };
    std::vector<Item> items;
    for (const std::string &wire : wires) {
        std::string suffix;
        if (!AnnounceSuffixOf(wire, prefix, &suffix)) continue;   /* 不在 scope 里 */
        Item it;
        it.wire = wire;
        it.suffix = suffix;
        items.push_back(it);
    }

    LOG_INFO("[moq-lite] %s:%d ANNOUNCE_REQUEST prefix=\"%s\": %zu of %zu broadcast(s) "
             "in scope, replying active=%zu",
             remote_ip_.c_str(), remote_port_, prefix.c_str(), items.size(),
             wires.size(), items.size());

    std::vector<uint8_t> out;
    /* origin 发 0（= Hop::UNKNOWN，「没有身份」）。我们是内容的源头，没有上游
     * 可标识；对端把这个 id 盖到 hop 链上做环路检测，0 表示不参与。
     * ⚠️ 这条没实测过，是本轮的一个未定项。 */
    if (!MlBuildAnnounceOk(out, 0, items.size())) {
        ResetStream(sid, kStreamInternalError, st, "cannot encode announce ok");
        return false;
    }
    for (size_t i = 0; i < items.size(); i++) {
        /* cost {0,0}：源头，取它不要钱。**不能**用 Cost::MAX/DRAIN —— 那是
         * 「本连接正在下线，请优先选别人」。 */
        if (!MlAppendAnnounceStart(out, items[i].suffix, 0, 0)) {
            ResetStream(sid, kStreamInternalError, st, "cannot encode announce start");
            return false;
        }
    }

    /* -06 的 announce id 不带在线上：收侧按**同一条流上的出现顺序**从 0 数。
     * 这里按同一顺序记账，后续 ANNOUNCE_END 要引用它。 */
    PeerAnnounce &pa = peer_announces_[sid];
    pa.prefix = prefix;
    pa.next_id = 0;
    pa.ids.clear();
    for (size_t i = 0; i < items.size(); i++) {
        pa.ids[items[i].wire] = pa.next_id++;
    }

    /* fin=0：announce 流要一直开着，后面的 PUBLISH/UNPUBLISH 更新还走它。 */
    if (QuicConnectionStreamSend(conn_, sid, out.data(), out.size(), 0) < 0) {
        LOG_ERROR("[moq-lite] %s:%d cannot send announce reply on stream %llu",
                  remote_ip_.c_str(), remote_port_,
                  static_cast<unsigned long long>(sid));
        return false;
    }
    return true;
}

void MoqLiteSession::SetAnnounceProvider(AnnounceProvider provider) {
    announce_provider_ = std::move(provider);
}

void MoqLiteSession::AnnounceBroadcast(const std::string &wire, bool added) {
    if (dead_ || wire.empty() || peer_announces_.empty()) return;

    for (auto &kv : peer_announces_) {
        const uint64_t sid = kv.first;
        PeerAnnounce &pa = kv.second;
        std::vector<uint8_t> out;

        if (added) {
            if (pa.ids.find(wire) != pa.ids.end()) continue;   /* 已经报过 */
            std::string suffix;
            if (!AnnounceSuffixOf(wire, pa.prefix, &suffix)) continue;
            if (!MlAppendAnnounceStart(out, suffix, 0, 0)) continue;
            if (QuicConnectionStreamSend(conn_, sid, out.data(), out.size(), 0) < 0) {
                LOG_DEBUG("[moq-lite] %s:%d cannot push ANNOUNCE_START on %llu",
                          remote_ip_.c_str(), remote_port_,
                          static_cast<unsigned long long>(sid));
                continue;
            }
            pa.ids[wire] = pa.next_id++;
            LOG_INFO("[moq-lite] %s:%d ANNOUNCE_START pushed: broadcast=\"%s\" "
                     "suffix=\"%s\" id=%llu",
                     remote_ip_.c_str(), remote_port_, wire.c_str(),
                     suffix.c_str(),
                     static_cast<unsigned long long>(pa.ids[wire]));
        } else {
            auto it = pa.ids.find(wire);
            if (it == pa.ids.end()) continue;
            const uint64_t ann_id = it->second;
            if (!MlAppendAnnounceEnd(out, ann_id)) continue;
            if (QuicConnectionStreamSend(conn_, sid, out.data(), out.size(), 0) < 0) {
                pa.ids.erase(it);
                continue;
            }
            pa.ids.erase(it);
            LOG_INFO("[moq-lite] %s:%d ANNOUNCE_END pushed: broadcast=\"%s\" id=%llu",
                     remote_ip_.c_str(), remote_port_, wire.c_str(),
                     static_cast<unsigned long long>(ann_id));
        }
    }
}

/* Subscribe 流（bidi 0x2）—— 对端要订某条 track。
 *
 * ⚠️ **-06 上不存在 SUBSCRIBE_OK**：接受是**隐式**的，第一条应答就是
 * SUBSCRIBE_START（lite/subscribe.rs:490-518）。拒绝了才 reset 那条流。
 * 而且 SUBSCRIBE_START **要等第一个组真的要写出去时才发** —— 那时才知道起始
 * 组号，里面的 group 才是诚实的（照参考实现的做法）。所以这里只登记，不发。 */
/* catalog.json 轨的下行。
 *
 * catalog 不是媒体：它的**一组一帧、帧载荷就是裸 JSON**（外层帧时间戳 0），
 * 而且没有 Legacy 那层微秒前缀（`hang/src/catalog/consumer.rs:95-99` 就是这么
 * 发的）。所以它不走 MoqLiteEgress，这里一次性发完：
 *   SUBSCRIBE_START{0}  →  一条 uni 流：[STREAM_TYPE 0x0][GROUP 头][FRAME] + FIN
 *
 * ⚠️ 组号从 0 起。catalog 后续变化由 MediaStreamManager 的 catalog 回调转发进
 * OnCatalogUpdated，补发**下一组**（seq=1、2…）—— 对端按组号去重，重发旧组
 * 会被当成重复丢掉。 */
void MoqLiteSession::ServeCatalog(uint64_t sid, uint64_t sub_id, RecvStream &st,
                                  const std::string &local_key) {
    std::string json;
    uint64_t rev = 0;
    if (!MediaStreamManager::GetCatalogJson(local_key, json, &rev)) {
        LOG_WARN("[moq-lite] %s:%d catalog subscribe %llu: no stored catalog for "
                 "key=\"%s\" (publisher gone?)",
                 remote_ip_.c_str(), remote_port_,
                 static_cast<unsigned long long>(sub_id), local_key.c_str());
        ResetStream(sid, kStreamCancelled, st, "no catalog available");
        return;
    }

    std::vector<uint8_t> start;
    if (!MlBuildSubscribeStart(start, 0)) return;
    if (QuicConnectionStreamSend(conn_, sid, start.data(), start.size(), 0) < 0) {
        LOG_ERROR("[moq-lite] %s:%d cannot send SUBSCRIBE_START for catalog on %llu",
                  remote_ip_.c_str(), remote_port_,
                  static_cast<unsigned long long>(sid));
        return;
    }

    if (!SendCatalogGroup(sub_id, 0, json, local_key)) return;

    /* 记录基线（组号 + catalog rev）：后续更新只补发对方还没收到的那一版。 */
    auto it = serve_subs_.find(sub_id);
    if (it != serve_subs_.end()) {
        it->second.catalog_group = 0;
        it->second.catalog_rev = rev;
    }
}

bool MoqLiteSession::SendCatalogGroup(uint64_t sub_id, uint64_t group_seq,
                                      const std::string &json,
                                      const std::string &local_key) {
    const uint64_t usid = QuicConnectionStreamOpenUni(conn_);
    if (usid == UINT64_MAX) {
        /* 额度耗尽。catalog 这一组丢掉的话对端**整条流都解析不了**（没有
         * catalog 就不知道有哪些 rendition），所以这条不是「少一帧」而是致命
         * 的。只警告：不阻塞、不重试（重试需要一个定时器，而这条路径在唯一的
         * uv loop 上）。对端会看到 announce 里有这条广播但拉不出内容。 */
        LOG_ERROR("[moq-lite] %s:%d no uni stream credit for catalog group %llu of "
                  "subscribe %llu - peer may mis-parse this broadcast",
                  remote_ip_.c_str(), remote_port_,
                  static_cast<unsigned long long>(group_seq),
                  static_cast<unsigned long long>(sub_id));
        return false;
    }

    std::vector<uint8_t> body;
    MlAppendVarint(body, 0x0);                       /* STREAM_TYPE = Group */
    if (!MlBuildGroupHeader(body, sub_id, group_seq, 0) ||   /* frame_start=0 */
        !MlAppendFrame(body, 0, reinterpret_cast<const uint8_t *>(json.data()),
                       json.size())) {
        QuicConnectionStreamReset(conn_, usid, 0x0);
        return false;
    }
    /* 一次性发完并 FIN：catalog 一组就一帧，没有后续。 */
    if (QuicConnectionStreamSendEx(conn_, usid, body.data(), body.size(), 1,
                                   nullptr, nullptr, 0) < 0) {
        LOG_ERROR("[moq-lite] %s:%d cannot write catalog group on uni stream %llu",
                  remote_ip_.c_str(), remote_port_,
                  static_cast<unsigned long long>(usid));
        return false;
    }
    LOG_INFO("[moq-lite] %s:%d catalog group sent: sub=%llu group=%llu key=\"%s\" "
             "bytes=%zu (one frame, raw JSON, FIN)",
             remote_ip_.c_str(), remote_port_,
             static_cast<unsigned long long>(sub_id),
             static_cast<unsigned long long>(group_seq),
             local_key.c_str(), json.size());
    return true;
}

void MoqLiteSession::OnCatalogUpdated(const std::string &stream_key, uint64_t rev) {
    if (dead_ || stream_key.empty()) return;

    std::string json;
    uint64_t cur_rev = 0;
    if (!MediaStreamManager::GetCatalogJson(stream_key, json, &cur_rev)) return;
    if (cur_rev < rev) rev = cur_rev;   /* 防御：调用方给的 rev 不应超前 */

    /* 先收集目标再逐个写：写 QUIC 可能间接触发会话关闭 / serve_subs_ 变更，
     * 不能边遍历边写。 */
    std::vector<uint64_t> targets;
    for (const auto &kv : serve_subs_) {
        const ServeSub &s = kv.second;
        if (s.track != kCatalogTrack) continue;
        if (s.key != stream_key) continue;
        if (s.catalog_rev >= rev) continue;   /* 这一版已经发给它了 */
        targets.push_back(kv.first);
    }

    for (uint64_t sub_id : targets) {
        auto it = serve_subs_.find(sub_id);
        if (it == serve_subs_.end()) continue;
        const uint64_t next_group = it->second.catalog_group + 1;
        if (!SendCatalogGroup(sub_id, next_group, json, stream_key)) continue;
        it->second.catalog_group = next_group;
        it->second.catalog_rev = rev;
    }

    if (!targets.empty()) {
        LOG_INFO("[moq-lite] %s:%d catalog update key=\"%s\" rev=%llu -> %zu "
                 "subscriber(s) got a new group",
                 remote_ip_.c_str(), remote_port_, stream_key.c_str(),
                 (unsigned long long)rev, targets.size());
    }

    /* 广播变得可公告：向对端开着的 announce 流补发 START（幂等）。
     * 这修的是「订阅方先连、推流端后起」那个缺口：以前只答快照。 */
    AnnounceBroadcast(AnnounceWireOf(stream_key), true);

    /* 回源挂起的订阅：catalog 到了就重试。放在最后，前面的补组逻辑先跑完。 */
    RetryPendingPeerSubscribes(stream_key);
}

void MoqLiteSession::TeardownEgress(const char *why) {
    for (auto &item : egress_) {
        if (!item.second) continue;
        item.second->Stop(why);
        MediaStreamManager::RemovePlayer(item.second.get());
    }
    egress_.clear();
}

void MoqLiteSession::ParsePeerSubscribe(uint64_t sid, RecvStream &st) {
    if (st.done) {
        st.buf.clear();
        return;
    }
    if (!publisher_armed_) {
        ResetStream(sid, kStreamCancelled, st, "subscribe but peer is not a subscriber");
        st.done = true;
        st.buf.clear();
        return;
    }

    size_t off = 0, blen = 0;
    const MlMsgStatus s = MlReadMessage(st.buf.data(), st.buf.size(), off, blen);
    if (s == kMlMsgTooLarge) {
        ResetStream(sid, kStreamInternalError, st, "subscribe over max message size");
        st.done = true;
        st.buf.clear();
        return;
    }
    if (s == kMlMsgNeedMore) {
        if (st.fin) {
            LOG_WARN("[moq-lite] %s:%d subscribe stream %llu ended inside its request",
                     remote_ip_.c_str(), remote_port_,
                     static_cast<unsigned long long>(sid));
            st.done = true;
            st.buf.clear();
        }
        return;
    }

    const size_t end = off + blen;
    const uint8_t *p = st.buf.data();
    size_t pos = off;

    /* 字段顺序（lite/subscribe.rs:57-90 的 decode_msg）：
     *   id(i) broadcast(s) track(s) priority(u8)
     *   [max_age(i) start_group(i) end_group(i)]
     *   [start_frame(i) end_frame(i)]
     *
     * ⚠️ 三处极易错的地方：
     *  ① lite-06 **没有** group-order 字节（version.rs:164-169），照 -05 抄会
     *     整个错位；
     *  ② start_group 在 -06 上是**绝对下界**（0 = 不限），**不是** Option；
     *     而 end_group / end_frame 是 Option，编码是 `0=None, N+1=Some(N)`
     *     （coding/encode.rs:174-184）—— 这两个是 N+1 形式，差一就是少订一组；
     *  ③ priority 是 u8 不是 varint。 */
    uint64_t sub_id = 0, priority = 0, max_age = 0;
    uint64_t start_group = 0, end_group_raw = 0, start_frame = 0, end_frame_raw = 0;
    std::string broadcast, track;
    /* priority 是**裸 u8**，不走 varint —— 单独读一个字节。
     * `pos < end` 这一步不能省：它是唯一挡住「串读越界」的检查，省掉的话
     * priority 会读到消息体之外的下一个字段上，后面整条链错位但不报错。 */
    bool ok = MlReadVarint(p, end, pos, sub_id) &&
              MlReadString(p, end, pos, broadcast) &&
              MlReadString(p, end, pos, track) &&
              pos < end;
    if (ok) priority = p[pos++];
    ok = ok && MlReadVarint(p, end, pos, max_age) &&
         MlReadVarint(p, end, pos, start_group) &&
         MlReadVarint(p, end, pos, end_group_raw) &&
         MlReadVarint(p, end, pos, start_frame) &&
         MlReadVarint(p, end, pos, end_frame_raw) &&
         pos == end;
    if (!ok) {
        ResetStream(sid, kStreamInternalError, st, "malformed subscribe request");
        st.done = true;
        st.buf.clear();
        return;
    }
    st.buf.erase(st.buf.begin(), st.buf.begin() + static_cast<std::ptrdiff_t>(end));
    st.done = true;   /* 一条流一个订阅：请求读完了，这条流我们不会再收东西 */

    /* subscribe id 是**会话内唯一**的（订阅方选的，我们原样回抄，GROUP 头里
     * 带的也是它）。撞号是会话级协议违规 —— 不拦的话两个订阅会共用一条
     * GROUP 的关联键，媒体静默串流。
     *
     * ⚠️ id 0 **不是保留号**，而且必是每个会话的第一个：参考实现的计数器是
     * `next_id: AtomicU64::default()`（= 0）加 `fetch_add(1)`
     * （`lite/subscriber.rs:115,3055`），发布端只把它当不透明键用、从不特判 0
     * （`lite/publisher.rs`）。这里曾经写成 `sub_id == 0 || …`，结果**每个会话
     * 的第一个订阅（永远是 id 0）都被我们当成撞号掐掉**，对端只能看到
     * 「duplicate subscribe id」。别再把它加回来。 */
    if (serve_subs_.find(sub_id) != serve_subs_.end() ||
        pending_peer_subs_.find(sub_id) != pending_peer_subs_.end()) {
        LOG_ERROR("[moq-lite] %s:%d subscribe id %llu already used in this session",
                  remote_ip_.c_str(), remote_port_,
                  static_cast<unsigned long long>(sub_id));
        CloseSession(kSessionProtocolViolation, "duplicate subscribe id");
        return;
    }

    PendingPeerSub pending;
    pending.sid           = sid;
    pending.sub_id        = sub_id;
    pending.broadcast     = broadcast;
    pending.track         = track;
    pending.priority      = priority;
    pending.max_age_ms    = max_age;
    pending.start_group   = start_group;
    pending.end_group_raw = end_group_raw;
    pending.start_frame   = start_frame;
    pending.end_frame_raw = end_frame_raw;

    std::string key, kind;
    if (!ResolveServeTrack(broadcast, track, &key, &kind)) {
        /* 本地没有这条流：如果集群层说「已发起回源」，先挂起 —— 对端那条
         * Subscribe bidi 流保持开着，catalog 一到就重试。 */
        const std::string app = routed_path_.empty() ? kFallbackApp
                                                     : routed_path_;
        pending.key = app + "/" + broadcast;
        /* 同 TRACK_INFO 分支：wire 用对端订阅的 broadcast，保留源端 app。 */
        if (on_stream_missing_ && on_stream_missing_(pending.key, broadcast)) {
            pending.deadline_ms = now_millisec() + deferred_subscribe_ms_;
            pending_peer_subs_[sub_id] = pending;
            LOG_INFO("[moq-lite] %s:%d SUBSCRIBE id=%llu key=\"%s\" track=\"%s\": "
                     "deferred %dms waiting for origin pull",
                     remote_ip_.c_str(), remote_port_,
                     static_cast<unsigned long long>(sub_id),
                     pending.key.c_str(), track.c_str(), deferred_subscribe_ms_);
            return;
        }

        /* 拒绝订阅 = **直接 reset 那条流**，不是发一条消息（-06 没有
         * SUBSCRIBE_ERROR 这种东西）。 */
        LOG_INFO("[moq-lite] %s:%d subscribe %llu rejected: broadcast=\"%s\" "
                 "track=\"%s\" not available",
                 remote_ip_.c_str(), remote_port_,
                 static_cast<unsigned long long>(sub_id), broadcast.c_str(),
                 track.c_str());
        ResetStream(sid, kStreamCancelled, st, "track not available");
        return;
    }

    pending.key = key;
    AttachPeerSubscribe(pending);
}

bool MoqLiteSession::AttachPeerSubscribe(PendingPeerSub sub) {
    auto sit = streams_.find(sub.sid);
    if (sit == streams_.end() || sit->second.reset_sent) {
        return false;
    }
    RecvStream &st = sit->second;

    std::string key, kind;
    if (!ResolveServeTrack(sub.broadcast, sub.track, &key, &kind)) {
        return false;
    }
    sub.key = key;

    ServeSub serve;
    serve.sid         = sub.sid;
    serve.broadcast   = sub.broadcast;
    serve.key         = key;
    serve.track       = sub.track;
    serve.priority    = sub.priority;
    serve.max_age_ms  = sub.max_age_ms;
    serve.start_group = sub.start_group;
    /* N+1 形式还原成原值；0 表示「不限」，我们恒发到 live 尽头，所以只记不使用 */
    serve.end_group   = sub.end_group_raw == 0 ? 0 : sub.end_group_raw - 1;
    serve.start_frame = sub.start_frame;
    serve.end_frame   = sub.end_frame_raw == 0 ? 0 : sub.end_frame_raw - 1;
    serve_subs_[sub.sub_id] = serve;

    LOG_INFO("[moq-lite] %s:%d SUBSCRIBE id=%llu key=\"%s\" track=\"%s\" kind=%s "
             "prio=%llu max_age=%llums groups=[%llu,%llu) frames=[%llu,%llu)",
             remote_ip_.c_str(), remote_port_,
             static_cast<unsigned long long>(sub.sub_id), key.c_str(),
             sub.track.c_str(), kind.c_str(),
             (unsigned long long)sub.priority, (unsigned long long)sub.max_age_ms,
             (unsigned long long)sub.start_group,
             (unsigned long long)serve.end_group,
             (unsigned long long)sub.start_frame,
             (unsigned long long)serve.end_frame);

    /* ── 接上下行 ──
     * catalog 不是媒体，单独发（见 ServeCatalog）；媒体轨造一个下行写入器，
     * 注册成 MediaStreamManager 的 player 之后回放 + 转 live。
     *
     * ⚠️ `ServeCatalog` / `Stop` / `ResetStream` 都可能写 QUIC 甚至（间接）
     * 关会话，之后**立刻 return**，绝不再碰 st —— st 是 streams_ 里的元素，
     * Parse 尾部还有一句 `if (st.done) st.buf.clear();`。这里 st.done 已经
     * 置真，所以就算返回也安全。 */
    if (kind == "catalog") {
        ServeCatalog(sub.sid, sub.sub_id, st, key);
        return true;
    }

    const bool is_video = (kind == "video");
    char peer[64];
    snprintf(peer, sizeof(peer), "%s:%d", remote_ip_.c_str(), remote_port_);
    char writer_id[128];
    /* writer id 必须**会话内唯一**：MediaStreamManager 按它摘牌，两个订阅共用
     * 一个 id 时后注册的把先注册的顶掉，先注册的那条订阅就永远收不到包。 */
    snprintf(writer_id, sizeof(writer_id), "%s#%llu", owner_id_.c_str(),
             static_cast<unsigned long long>(sub.sub_id));

    auto eg = std::unique_ptr<MoqLiteEgress>(
        new MoqLiteEgress(conn_, key, writer_id, sub.track, peer, is_video));
    MoqLiteEgress *raw = eg.get();
    egress_[sub.sub_id] = std::move(eg);

    /* 预置 init_flag_：绕开 AddPlayer 内建的 WriterGop 回放（GOP 由我们自己的
     * SendGop 发，见 MoqLiteEgress::Start）。与 MoqPushSession 同一套做法。 */
    raw->SetInitFlag(true);
    MediaStreamManager::AddPlayer(raw);
    /* 两个号都要给：sub_id 是**对端选的订阅号**（GROUP 头里报的是它），sid 是
     * 对端开的那条 Subscribe bidi 流（SUBSCRIBE_START 写它、Stop 时 reset 它）。 */
    raw->Start(sub.sub_id, sub.sid);
    return true;
}

void MoqLiteSession::RetryPendingPeerSubscribes(const std::string &stream_key) {
    if (pending_peer_subs_.empty()) return;

    std::vector<uint64_t> ids;
    for (const auto &kv : pending_peer_subs_) {
        if (kv.second.key == stream_key) ids.push_back(kv.first);
    }
    for (uint64_t id : ids) {
        auto it = pending_peer_subs_.find(id);
        if (it == pending_peer_subs_.end()) continue;
        PendingPeerSub sub = it->second;

        auto sit = streams_.find(sub.sid);
        if (sit == streams_.end() || sit->second.reset_sent) {
            pending_peer_subs_.erase(it);
            continue;
        }

        std::string key, kind;
        if (!ResolveServeTrack(sub.broadcast, sub.track, &key, &kind)) {
            continue;   /* catalog 有了但这条轨还没有，继续等 */
        }
        pending_peer_subs_.erase(it);
        LOG_INFO("[moq-lite] %s:%d deferred SUBSCRIBE id=%llu key=\"%s\" "
                 "track=\"%s\": catalog ready, attaching",
                 remote_ip_.c_str(), remote_port_,
                 static_cast<unsigned long long>(sub.sub_id), key.c_str(),
                 sub.track.c_str());
        AttachPeerSubscribe(sub);
    }
}

void MoqLiteSession::SweepPendingPeerSubscribes(int64_t now_ms) {
    if (dead_ || pending_peer_subs_.empty()) return;

    for (auto it = pending_peer_subs_.begin(); it != pending_peer_subs_.end();) {
        if (now_ms < it->second.deadline_ms) {
            ++it;
            continue;
        }
        const uint64_t sid = it->second.sid;
        auto sit = streams_.find(sid);
        if (sit != streams_.end()) {
            ResetStream(sid, kStreamCancelled, sit->second,
                        "deferred subscribe timed out (origin pull)");
        } else {
            QuicConnectionStreamReset(conn_, sid, kStreamCancelled);
        }
        LOG_INFO("[moq-lite] %s:%d deferred SUBSCRIBE id=%llu key=\"%s\": "
                 "timed out after %dms",
                 remote_ip_.c_str(), remote_port_,
                 static_cast<unsigned long long>(it->second.sub_id),
                 it->second.key.c_str(), deferred_subscribe_ms_);
        it = pending_peer_subs_.erase(it);
    }
}

void MoqLiteSession::ParseBidiStream(uint64_t sid, RecvStream &st) {
    /* 对端主动开的 bidi 流。bidi 的 creator 是 subscriber，我们是服务端，
     * 所以这只可能是 Goaway 这类对端发起的控制流 —— M2 仍未处理，记录即可。
     * （我们**自己**开的 bidi 流不会走到这里：Parse 按 st.local 先分走了。） */
    if (!st.done) {
        st.done = true;
        LOG_WARN("[moq-lite] %s:%d peer-initiated bidi stream %llu type=0x%llx "
                 "(not handled yet; ignoring)",
                 remote_ip_.c_str(), remote_port_,
                 static_cast<unsigned long long>(sid),
                 static_cast<unsigned long long>(st.type));
    }
    st.buf.clear();
}

} /* namespace cpp_streamer */
