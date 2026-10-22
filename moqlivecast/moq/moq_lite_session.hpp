#ifndef MOQ_LITE_SESSION_HPP
#define MOQ_LITE_SESSION_HPP

#include "moq/moq_lite_egress.hpp"
#include "quic_connection.h"
#include "media_packet.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace cpp_streamer {

/* 一个已订阅的 track —— catalog 里的一条 rendition。
 *
 * 字段拼写**故意**跟着 catalog 的 camelCase 走（codedWidth / sampleRate /
 * numberOfChannels），因为那串名字是线上原文，翻译一层就多一处对不上的机会。
 * 注意 catalog 里**没有** timescale：它只存在于每个 track 的 TRACK_INFO 里，
 * 而 -06 的 FRAME 头带的差值是 track timescale 单位 —— 所以 M3 要知道单位
 * 得另想办法（见 docs/moq-transport-gap.md）。
 *
 * M3 拿 name + codec + codec_config 去建 MediaStream 与 FLV 序列头。 */
struct MoqLiteTrack {
    std::string name;                  /* rendition key == track 名，原样就能订阅 */
    std::string kind;                  /* "video" / "audio" / "text" / … */
    std::string codec;                 /* avc1.PPCCLL / mp4a.40.2 */
    std::vector<uint8_t> codec_config; /* description：hex 解出来的 avcC / ASC */
    uint32_t width = 0;                /* video: codedWidth */
    uint32_t height = 0;               /* video: codedHeight */
    uint32_t sample_rate = 0;          /* audio: sampleRate */
    uint32_t channels = 0;             /* audio: numberOfChannels */
    uint64_t subscribe_id = 0;         /* 0 = 还没订阅 */
    bool     started = false;          /* 收到过 SUBSCRIBE_START */
    uint64_t groups = 0;               /* 只用于 M2/3 的日志与自检 */
    uint64_t frames = 0;

    /* ── M3 落地状态 ── */
    bool     m3_usable = false;        /* codec 是我们认得的（H.264 / AAC），才会往
                                        * MediaStreamManager 送 */
    bool     seq_hdr_sent = false;     /* FLV 序列头是不是已经发过 */
    bool     shape_logged = false;     /* 首帧形状（剥掉容器前缀后的前 16 字节）打过没 */
    bool     prefix_bad_warned = false;/* 帧短到装不下容器前缀，只警告一次 */
    bool     ts_mismatch_warned = false; /* 内外两层时间戳对不上，只警告一次 */
    uint64_t delivered = 0;            /* 真正送进 MediaStreamManager 的帧数 */
};

/* 一条 moq-lite 会话 —— binding 1（裸 QUIC，ALPN = moq-lite-06）。
 *
 * M1：传输打通 —— 自己开 Setup 流（uni 0x1）发空参数表 SETUP 随即 FIN；
 *     读对端的 Setup 流解出 Path 参数（§7.3.2）据此路由。
 *
 * M2：协商打通 —— 路由定了之后自动往下走：
 *     ① 开两条 Announce 流（bidi 0x1）发 ANNOUNCE_REQUEST，prefix 一条 `""`
 *        一条 `routed_path_`（R8：发错是**静默**的，两条都发才能一轮定案）；
 *     ② 拿到 broadcast 全路径后订阅 `catalog.json`（bidi 0x2）；
 *     ③ 解 catalog JSON，对每条 rendition 各开一条 Subscribe 流；
 *     ④ 收 GROUP（uni 0x0）+ FRAME，catalog 的帧当场解 JSON，
 *        媒体帧只记计数与首帧形状（真正落地是 M3）。
 *
 * ⚠️ **本端发起的 bidi 流，对端的应答不带 STREAM_TYPE** —— 类型字节是
 * **发起方**写的（-06 §5.1：bidi 流的 creator 是 subscriber）。所以
 * RecvStream::local 记录「这条流是我们开的」，Parse 据此跳过类型读取。
 * 少了这一步，ANNOUNCE_OK 的首字节会被当成流类型解掉。
 *
 * 线程：全部回调都在 QUIC loop 线程里，故内部不做加锁。
 * 生命周期：对象归 MoqLiteServer 所有；连接关闭时由本类的 close 回调通知上层回收。 */
class MoqLiteSession {
public:
    /* 对端 SETUP 收齐、Path 解出来时回调。返回 false = 不认这个路径，
     * 本类会按 §7.3.2 关掉会话。 */
    using OnRoute = std::function<bool(MoqLiteSession &sess)>;
    /* 会话终结（对端关闭 / 我们关的 / 出错）。回调返回后本对象可能已被销毁，
     * 实现里不要在这之后再碰 sess。 */
    using OnClose = std::function<void(MoqLiteSession &sess)>;
    /* 本地缺流时的回源钩子（cluster edge）。返回 true = 已经发起回源，
     * SUBSCRIBE 可以挂起一段时间等 catalog；false / 未注册 = 直接拒绝。
     * local_key 是本端拼出来的 key（routed_path/broadcast），wire 是对端
     * SUBSCRIBE 里的 broadcast —— 远端广播已经是 app-qualified 的名字
     * （如 live1/stream1），回源必须按它去 Center 找，不能用本端路由路径。 */
    using OnStreamMissing =
        std::function<bool(const std::string &local_key, const std::string &wire)>;

    MoqLiteSession(QuicConnection *conn,
                   std::string alpn,
                   std::string remote_ip, int remote_port,
                   OnRoute on_route, OnClose on_close,
                   OnStreamMissing on_stream_missing = nullptr,
                   int deferred_subscribe_ms = 3000);
    ~MoqLiteSession();

    MoqLiteSession(const MoqLiteSession &) = delete;
    MoqLiteSession &operator=(const MoqLiteSession &) = delete;

    /* 挂回调 + 开自己的 Setup 流。构造完立刻调。 */
    void Start();

    /* 回源挂起中的 SUBSCRIBE 超时清理（由 MoqLiteServer 的 tick 调用）。 */
    void SweepPendingPeerSubscribes(int64_t now_ms);

    /* 本地可公告的广播发生变化时，向**对端开的 announce 流**补发
     * ANNOUNCE_START / ANNOUNCE_END。wire 是线上广播名（带 '/' 的相对路径）。
     * 幂等：同一条已经在 ids 里就不会重复发。 */
    void AnnounceBroadcast(const std::string &wire, bool added);

    /* 额外的可公告集合（集群 edge 的远端广播）。只在回答 ANNOUNCE_REQUEST
     * 时合并进来；变化仍然走 AnnounceBroadcast。 */
    using AnnounceProvider = std::function<void(std::vector<std::string> *)>;
    void SetAnnounceProvider(AnnounceProvider provider);

    QuicConnection *Conn() const { return conn_; }
    const std::string &Alpn() const { return alpn_; }
    const std::string &RemoteIp() const { return remote_ip_; }
    int RemotePort() const { return remote_port_; }

    /* 对端 SETUP 的 Path 参数原值（含 URI query）。未收到 / 对端没发 → 空串。 */
    const std::string &Path() const { return path_raw_; }
    /* Path 里的路径部分（去掉 ?query，去掉前导 '/'）。 */
    const std::string &PathOnly() const { return path_only_; }
    /* Path 里的 query 部分（不含 '?'）。没有 → 空串。 */
    const std::string &Query() const { return query_; }
    /* 路由命中的路径（对端没发 Path 时是服务端默认路径）。
     * 由上层在 OnRoute 里定下来 —— 会话自己不知道有哪些路径可路由。 */
    void SetRoutedPath(const std::string &p) { routed_path_ = p; }
    const std::string &RoutedPath() const { return routed_path_; }
    bool PeerSetupReceived() const { return peer_setup_done_; }
    bool Dead() const { return dead_; }

    /* ── M2 协商的结果 ── */

    /* announce 定下来的 broadcast 全路径（prefix + '/' + suffix）。空 = 还没定。 */
    const std::string &BroadcastPath() const { return broadcast_; }
    /* catalog 里认出来的 track，key = track 名（就是 rendition 的 key）。M3 用。 */
    const std::map<std::string, MoqLiteTrack> &Tracks() const { return tracks_; }
    /* catalog 的 clock 节：wall = PTS 0 的 epoch 毫秒，timescale 通常 1000000。
     * M3 拼绝对时间轴要用。0 = catalog 里没带这一节。 */
    uint64_t ClockWall() const { return clock_wall_; }
    uint64_t ClockTimescale() const { return clock_timescale_; }

    /* 主动关会话（会话错误码取 -06 §4.4.1） */
    void CloseSession(uint64_t error_code, const char *reason);

    /* catalog.json 内容更新（MediaStreamManager 的 rev 自增）时由上层转发：
     * 给本会话里订阅了这条 key 的 catalog 订阅方各补发一个新组。
     * 同一条 loop 线程；与订阅解析、下行写入不会并发。 */
    void OnCatalogUpdated(const std::string &stream_key, uint64_t rev);

    /* ── M3：媒体落地 ── */

    /* 帧要落进的流 key，形如 "<路由路径>/<broadcast 路径>"。
     * 空 = 还没定下 broadcast，或者根本没有媒体轨可用。
     *
     * ⚠️ 这里的 app 段是**服务端的入口路径**（`moqlite_subpath` 里配的那条，
     * 也就是客户端拨的 URL 最后一段），不是发布端的 origin —— 实测 moq-cli
     * 的 SETUP 不带 Path 参数，它的 broadcast 命名空间**根在 ""**（这条是
     * ANNOUNCE 的实测结论：发 prefix="" 才有回音，发 "anon" 回空集合）。
     * 所以 app 段只能由服务端自己定；好在它本来就是配置项，
     * moqlite_subpath 写 live 就是 live/xxx，不必再加旋钮。 */
    const std::string &StreamKey() const { return stream_key_; }

private:
    /* 单条接收流的状态。moq-lite 的流都是「先一个 STREAM_TYPE，再流类型自己的
     * 消息」，所以头一字节要跨多次回调才能读全 —— 但**本端发起的 bidi 流是
     * 例外**：类型字节是我们自己写的，对端在同一根流上回的应答上来就是消息，
     * 见 local。 */
    struct RecvStream {
        std::vector<uint8_t> buf;   /* 还没被消费掉的字节 */
        uint64_t type = 0;
        bool     typed = false;     /* STREAM_TYPE 已解出（local 流建流时就为真）*/
        bool     local = false;     /* 本端发起的 bidi 流：应答不带 STREAM_TYPE */
        bool     fin = false;       /* 对端已 FIN */
        bool     done = false;      /* 流类型自己的解析已收尾 */
        bool     reset_sent = false;

        /* ── 本端发起的控制流（Announce 0x1 / Subscribe 0x2）── */
        uint64_t stage = 0;         /* 见 .cpp 的 kAnnStage* / kSubStage* */
        uint64_t expect = 0;        /* Announce：ANNOUNCE_OK 说还该收几条 START */
        uint64_t ann_next_id = 0;   /* Announce：隐式 ID 计数器（每条 START 递增）*/
        uint64_t sub_id = 0;        /* Subscribe：我们选的那个 Subscribe ID */
        std::string req_path;       /* Announce：请求的 prefix；Subscribe：broadcast */
        std::string req_track;      /* Subscribe：track 名 */

        /* ── GROUP（uni 0x0）── */
        bool     group_hdr_done = false;
        uint64_t sub_id_in_group = 0;
        uint64_t group = 0;
        uint64_t frame_index = 0;
        int64_t  ts_acc = 0;        /* 组内累加时间（track timescale 单位）*/
        /* 这个组里「关键帧还没出现」。组的 frame_start == 0 时才置位 ——
         * frame_start > 0 说明我们是从组中间开始收的，那第一帧本来就不是关键帧。
         * 用标志位而不是 frame_index == 0：空载荷哨兵也会占 frame_index。 */
        bool     key_pending = false;
    };

    void SendSetup();

    /* ── M2：协商 ── */
    void StartDiscovery();
    bool OpenCtrlStream(uint64_t stream_type, uint64_t *out_sid);
    bool WriteMessage(uint64_t sid, bool typed, uint64_t type,
                      const std::vector<uint8_t> &body, bool fin);
    void StartAnnounce(const std::string &prefix);
    void Subscribe(const std::string &broadcast, const std::string &track,
                   uint64_t priority);
    void ParseAnnounceStream(uint64_t sid, RecvStream &st);
    void ParseAnnounceBody(RecvStream &st, uint64_t type, const uint8_t *body,
                           size_t len, bool *bad, std::string *why);
    void ParseSubscribeStream(uint64_t sid, RecvStream &st);
    void OnAnnounceRoute(const std::string &prefix, const std::string &suffix);
    void OnCatalogFrame(const uint8_t *payload, size_t len);
    void OnMediaFrame(uint64_t sid, RecvStream &st, const uint8_t *payload,
                      size_t len);

    /* ── M3：媒体落地 ── */
    void BuildStreamKey();
    /* 首次要写盘时认领这条流（同名流新顶旧）。返回 false = 不该继续写。 */
    bool EnsureStreamClaimed();
    /* 每次写之前的 owner 校验，被顶掉就丢包（按秒限流打日志）。 */
    bool IsStreamOwner();
    void ReleaseStream();
    void EmitFlvVideoSeqHdr(MoqLiteTrack &ti, int64_t ts_ms);
    void EmitFlvAudioSeqHdr(MoqLiteTrack &ti, int64_t ts_ms);
    void EmitFlvVideoFrame(MoqLiteTrack &ti, int64_t ts_ms, bool seq, bool key,
                           const uint8_t *data, size_t len);
    void EmitFlvAudioFrame(MoqLiteTrack &ti, int64_t ts_ms, bool seq,
                           const uint8_t *data, size_t len);
    /* owner 校验 + WriterMediaPacket + 计数。所有落地包都走这一个出口。 */
    bool Deliver(const char *what, Media_Packet_Ptr pkt);

    void HandleStreamData(uint64_t sid, const uint8_t *data, size_t len, int fin);
    void Parse(uint64_t sid, RecvStream &st);
    void OnStreamTypeSeen(uint64_t sid, RecvStream &st);
    void ParseSetupStream(uint64_t sid, RecvStream &st);
    void ParseBidiStream(uint64_t sid, RecvStream &st);
    void ParseGroupStream(uint64_t sid, RecvStream &st);
    void OnSetupParam(uint64_t id, const uint8_t *val, size_t len,
                      bool *bad_format, std::string *bad_reason);
    void ResetStream(uint64_t sid, uint64_t error_code, RecvStream &st,
                     const char *why);

    static void OnStreamDataTrampoline(QuicConnection *conn, uint64_t stream_id,
                                       const uint8_t *data, size_t len, int fin);
    static void OnCloseTrampoline(QuicConnection *conn, uint64_t error_code,
                                  const char *reason);

    /* conn → session。C 侧回调只有 conn 一个抓手，没有 user_data，所以这里
     * 自己存一张表。只在 QUIC loop 线程访问。 */
    static std::map<QuicConnection *, MoqLiteSession *> &Registry();

    QuicConnection *conn_ = nullptr;
    std::string alpn_;
    std::string remote_ip_;
    int         remote_port_ = 0;

    /* 一个挂起的 SUBSCRIBE：请求已完整解析，但本地还没有 catalog（正在回源）。 */
    struct PendingPeerSub {
        uint64_t sid = 0;
        uint64_t sub_id = 0;
        std::string broadcast;
        std::string track;
        std::string key;
        uint64_t priority = 0;
        uint64_t max_age_ms = 0;
        uint64_t start_group = 0;
        uint64_t end_group_raw = 0;
        uint64_t start_frame = 0;
        uint64_t end_frame_raw = 0;
        int64_t  deadline_ms = 0;
    };

    /* 对端开的 announce 流：请求前缀 + 已公告的 (广播全名 → 隐式 announce id)。
     * -06 的 announce id 不带在线上，收侧按出现顺序自己数，所以每条流各自一份
     * 从 0 开始的计数器。 */
    struct PeerAnnounce {
        std::string prefix;
        uint64_t next_id = 0;
        std::map<std::string, uint64_t> ids;
    };
    std::map<uint64_t, PeerAnnounce> peer_announces_;   /* sid → 状态 */

    OnRoute on_route_;
    OnClose on_close_;
    OnStreamMissing on_stream_missing_;
    AnnounceProvider announce_provider_;
    int deferred_subscribe_ms_ = 3000;
    /* sub_id → 挂起中的订阅。回源拿到 catalog 后在这里重试。 */
    std::map<uint64_t, PendingPeerSub> pending_peer_subs_;

    std::map<uint64_t, RecvStream> streams_;
    bool peer_setup_done_ = false;
    bool saw_peer_setup_  = false;   /* 见过 Setup 流（不要求解成功） */
    bool dead_ = false;

    std::string path_raw_;
    std::string path_only_;
    std::string query_;
    std::string routed_path_;

    /* ── M2 ──
     * subscribe id 是**订阅方选的**、会话内唯一的，发布端原样回抄，并且每个
     * GROUP 头里带的就是它 —— 这是 GROUP 流关联到 track 的**唯一**手段
     * （没有 alias、没有 object id）。 */
    std::map<uint64_t, std::string> subs_;      /* subscribe id → track 名 */
    std::map<std::string, MoqLiteTrack> tracks_;/* track 名 → 配置 */
    std::string broadcast_;
    uint64_t next_subscribe_id_ = 1;   /* 从 1 起，0 留给「没有订阅」 */
    bool discovery_started_ = false;
    bool catalog_subscribed_ = false;
    uint64_t catalog_sub_id_ = 0;
    uint64_t clock_wall_ = 0;
    uint64_t clock_timescale_ = 0;

    /* ── M3：媒体落地 ──
     * 这条 moq-lite 会话落进 MediaStreamManager 的身份。owner_id_ 要能和别的
     * 会话区分开（顶替判定靠它），所以带一个进程内递增序号 —— 光用 ip:port
     * 不够：同一客户端重连时源端口可能复用，那时会把「旧会话还在」误判成自己。 */
    std::string stream_key_;      /* "<路由路径>/<broadcast>" */
    std::string stream_app_;      /* key 的第一段，填进 Media_Packet::app_ */
    std::string stream_name_;     /* key 剩下的部分，填进 Media_Packet::streamname_ */
    std::string owner_id_;
    bool     stream_claimed_ = false;
    int64_t  owner_drop_warn_s_ = 0;   /* 丢包日志的按秒限流 */

    /* catalog 的**原文**（剥掉容器前缀后逐字节保留）。
     * 到达顺序不定：catalog 常常早于第一个媒体包，而那时 MediaStream 还没被
     * claim 出来（它由 AddPublisher 在第一个媒体包上创建），直接写管理器会
     * 找不到目标。所以先存这里，claim 成功后（EnsureStreamClaimed）再推过去。
     * 发布端角色（moq-lite export）要原样用这份 —— 从 FLV 序列头反推不出
     * codec 串和 clock。 */
    std::string catalog_json_raw_;

    /* 把 catalog_json_raw_ 推给 MediaStreamManager（幂等：内容没变就是空操作）。
     * 订阅端与发布端两个角色都要用它。 */
    void PushCatalogToManager();

    /* ── M4：发布端角色 ──
     * 同一条连接上的**反方向**：对端是 subscriber，我们应答并往下发。
     * 与 M2/M3 共用一套解码器与缓冲区，但状态是分开的（见 serve_subs_）。 */

    /* 一条「对端订阅我们」的订阅。
     *
     * ⚠️ 与 subs_ **必须分开存**：那张表装的是「我们自己选的 subscribe id」
     * （我们当订阅者时选的），这张装的是「对端选的」。两个 id 空间各自从 1
     * 起编，混在一张表里必然撞号，而且撞了以后是静默串流。 */
    struct ServeSub {
        uint64_t sid = 0;          /* 请求所在的那条 bidi 流（应答写回这里）*/
        std::string broadcast;     /* 对端请求的 broadcast 全名 */
        std::string key;           /* 解析后的本地 key（catalog 更新按它匹配） */
        std::string track;         /* catalog.json 或 rendition key */
        uint64_t priority = 0;
        uint64_t max_age_ms = 0;
        /* lite-06 的组界限：start 是**绝对下界**（0 = 不限），
         * end/frame_end 是 N+1 形式（0 = 不限）。 */
        uint64_t start_group = 0;
        uint64_t end_group = 0;
        uint64_t start_frame = 0;
        uint64_t end_frame = 0;
        /* SUBSCRIBE_START **要等第一个组真的要写出去时才发** —— 那时才知道
         * 起始组号，里面的 group 才是诚实的（照参考实现的做法）。 */
        bool     started = false;
        uint64_t first_group = 0;
        uint64_t groups = 0;
        uint64_t frames = 0;
        /* catalog.json 订阅专用：已经发到第几组 / 对应哪一版 catalog。
         * 更新时据此只补发“对方还没收到”的那一版，不重复发。 */
        uint64_t catalog_group = 0;
        uint64_t catalog_rev = 0;
    };

    /* 对端 SETUP 里 Role 参数的原值：0 Both（默认）/ 1 Publisher / 2 Subscriber。
     * 语义是**对端自己是谁**。对端 = 2（只想订阅）时我们**不能**发
     * ANNOUNCE_REQUEST —— 那是订阅方的动作，订阅端-only 的对端会把那条流
     * reset 掉，白噪音。 */
    uint64_t peer_role_ = 0;
    bool     publisher_armed_ = false;
    std::map<uint64_t, ServeSub> serve_subs_;   /* 对端选的 sub id → 订阅 */
    /* 媒体轨的下行写入器，一个订阅一个（见 MoqLiteEgress 的说明）。
     * 用 unique_ptr：MoqLiteEgress 不可拷贝，且生存期严格跟随本会话。 */
    std::map<uint64_t, std::unique_ptr<MoqLiteEgress>> egress_;
    /* 停掉并摘除全部下行写入器。会话关闭 / 对端撤销订阅时调，幂等。 */
    void TeardownEgress(const char *why);
    /* 组号不在这里：它是**按 track** 编的，不是按订阅 —— 同一条 track 的两个
     * 订阅方必须看到同一批组号，否则各自的时间轴对不上。所以那个计数器属于
     * 步骤 4 的每条 track 的下行组写入器，不放在会话上。 */

    /* 切到发布端角色：只记牌子，不主动发任何东西（与 StartDiscovery 相反）。 */
    void ArmPublisher();
    /* 对端发起的 bidi 控制流（-06 §5.1：bidi 的 creator 是 subscriber，
     * 所以这三条只可能是对端开的）。 */
    void ParsePeerAnnounce(uint64_t sid, RecvStream &st);
    void ParsePeerSubscribe(uint64_t sid, RecvStream &st);
    /* catalog 就绪后真正建立下行（原 ParsePeerSubscribe 的收尾）。 */
    bool AttachPeerSubscribe(PendingPeerSub sub);
    void RetryPendingPeerSubscribes(const std::string &stream_key);
    void ParsePeerTrack(uint64_t sid, RecvStream &st);
    /* 把线上 broadcast 名解析成本地 key，并确认这条 track 真的存在。
     * 解析不出来 / 没有 catalog / catalog 里没这条 rendition → false
     * （调用方一律 reset 那条流，不关会话）。
     * kind 出参是 catalog 里那一节的 key（"video"/"audio"），catalog 轨本身
     * 是 "catalog" —— 下行写入器靠它决定过滤哪个 av_type。可为 nullptr。 */
    bool ResolveServeTrack(const std::string &broadcast, const std::string &track,
                           std::string *local_key, std::string *kind);
    /* 本地 key → 对端可见的广播名：
     *   <本端路由>/<app>/<stream>（Center 的集群推流）→ <app>/<stream>
     *   <app>/<stream>（Edge 本地推流 / Center 直接 ingest）→ 原样
     * 目的：公告永远带源端 app，回源方才能把 (app, stream) 身份原样带回。 */
    std::string AnnounceWireOf(const std::string &stream_key) const;
    /* catalog.json 轨的下行：一条 SUBSCRIBE_START + 一个单帧组（裸 JSON）。
     * catalog 不是媒体，不走 MoqLiteEgress。st 是那条订阅流（拒绝时 reset 它）。 */
    void ServeCatalog(uint64_t sid, uint64_t sub_id, RecvStream &st,
                      const std::string &local_key);
    /* 发一个单帧 catalog 组：一条新的 uni 流，[GROUP][FRAME(ts=0, 裸 JSON)] + FIN。
     * 组号从 0 起；更新时组号 +1（对端按组号去重，重发旧组会被忽略）。 */
    bool SendCatalogGroup(uint64_t sub_id, uint64_t group_seq,
                          const std::string &json, const std::string &local_key);
    /* 算一遍请求前缀之下可公告的广播，把 ANNOUNCE_OK + N 条 ANNOUNCE_START
     * 一次写出去。返回 false 时内部已经 reset 过那条流。 */
    bool AnnounceGrid(uint64_t sid, RecvStream &st, const std::string &prefix);
};

/* ============================================================
 * 各流类型的解析阶段（RecvStream::stage）
 *
 * 值域按流类型分开解释，所以两类都从 0 起。 */
enum MoqLiteAnnStage {
    kAnnStageWaitOk  = 0,   /* 等 ANNOUNCE_OK（普通消息，无 Type 判别符）*/
    kAnnStageRunning = 1,   /* 逐条收 START / END / UPDATE */
};
enum MoqLiteSubStage {
    kSubStageWaitResp = 0,  /* 等第一条应答 */
    kSubStageRunning  = 1,  /* 逐条收 START / END / DROP */
};
/* 对端发起的 announce 流（我们是应答方，方向反过来）。
 * 与上面两类分开：那两个的 stage 记的是**我们等对端应答**的进度。 */
enum MoqLitePeerAnnStage {
    kPeerAnnStageWaitRequest = 0,  /* 等那条唯一的 ANNOUNCE_REQUEST */
    kPeerAnnStageRunning     = 1,  /* 已应答，流保持打开等我们推更新 */
};

} /* namespace cpp_streamer */

#endif /* MOQ_LITE_SESSION_HPP */
