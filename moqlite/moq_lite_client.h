#ifndef MOQ_LITE_CLIENT_H
#define MOQ_LITE_CLIENT_H

/* ============================================================
 * moq-lite-06 客户端 —— C API
 *
 * 形态参考 libmoq 的 C API（`rs/libmoq/src/api.rs`）：**句柄是整数 id**，
 * 没有可供调用方解引用的结构体指针，所有对象都由库自己持有与回收。这样
 * 调用方不可能拿着一个已释放的指针（本库的主要失效模式就是「回调里把
 * 对象删掉之后又碰它」）。
 *
 * 承载是**裸 QUIC**（ALPN `moq-lite-06`），建立在 quic/quic_client_api 之上。
 * 与浏览器那条 WebTransport 路（h3）是两套绑定，不共用 —— 本 API 不做
 * WebTransport（没有 CONNECT、没有 WT 帧），所以 ALPN 也**不 offer h3**：
 * 两边都 offer h3 时协商会落到 h3，服务端于是按 WebTransport 处理，等一个
 * 永远不来的 CONNECT。**症状是「连上了但没有任何下文」**，很难查。
 *
 * ── 线程 ──
 * 所有回调都在**调用方传进来的那条 loop** 的线程里执行（见
 * moq_lite_session_new）。`_connect` 与 `_close`/`_unsubscribe` 可以从任意
 * 线程调，其余（`_announce`/`_subscribe`）**必须在 loop 线程调**。
 *
 * ── 事件循环 ──
 * **循环不归本库** —— 不挑一条、也不提供驱动接口。你把它传进来，你跑它，
 * 你决定什么时候停（那条 loop 上很可能还有你的定时器、信号、别的连接）。
 *
 * ── 生命周期 ──
 * 回调里**不要**释放会话或订阅 —— 返回之后再调 `_close`/`_unsubscribe`。
 * 库内部保证「回调期间对象有效、返回后可能已被下一步动作标记为死」。
 * ============================================================ */

#include <stddef.h>
#include <stdint.h>
#include <uv.h>   /* uv_loop_t —— 由调用方提供，本库不拥有循环 */

#ifdef __cplusplus
extern "C" {
#endif

/* 句柄。0 恒为「无效」（与 libmoq 的 0 号约定一致）。 */
typedef uint32_t moq_lite_session_t;
typedef uint32_t moq_lite_broadcast_t;
typedef uint32_t moq_lite_sub_t;
typedef uint32_t moq_lite_track_t;

/* ── 版本 / ALPN ────────────────────────────── */

/* 本库实现的版本号。默认只 offer 这一个 —— 见头文件顶部「不 offer h3」。 */
#define MOQ_LITE_VERSION "moq-lite-06"

/* 默认端口（URL 里没写端口时用）。 */
#define MOQ_LITE_DEFAULT_PORT 4433

/* ── 会话回调 ───────────────────────────────── */

typedef struct {
    /* 握手完成（status=0）或失败（status<0 见错误码）。
     * ⚠️ 握手成功**不代表**对端认这个 ALPN —— 要确认请用
     * moq_lite_session_alpn()。协商不到时对端会 alert 120，那时这里是失败。 */
    void (*on_connected)(moq_lite_session_t s, int status, void *user);

    /* 对端公告了一条路由。suffix 是相对请求 prefix 的路径（广播恰好等于
     * 请求前缀时是空串 —— 最常见的那种）。收到后即可 subscribe。 */
    void (*on_announce)(moq_lite_session_t s, moq_lite_broadcast_t bc,
                        const char *path, const char *suffix, void *user);

    /* 对端撤回了某条公告（ANNOUNCE_END）。bc 会随即失效，别再 subscribe。 */
    void (*on_announce_end)(moq_lite_session_t s, moq_lite_broadcast_t bc,
                            void *user);

    /* 会话终结（对端关闭 / 我们关的 / 出错）。**这是最后一次回调**，
     * 之后本会话的句柄全部失效（订阅也一并失效，不必再 cancel）。 */
    void (*on_close)(moq_lite_session_t s, int err, const char *reason,
                     void *user);

    void *user;
} moq_lite_session_cb_t;

/* ── 订阅回调 ───────────────────────────────── */

typedef struct {
    /* 对端接受订阅（SUBSCRIBE_START）。first_group 是它开始发的组号。
     * ⚠️ -06 上接受是**隐式**的，没有 SUBSCRIBE_OK —— 这条 START 就是接受。
     * 拒绝不是消息，是对端 reset 那条流；那时走 on_drop/on_close，不会走这里。 */
    void (*on_subscribe)(moq_lite_sub_t sub, uint64_t first_group, void *user);

    /* 一帧数据。payload/len 指向库内部缓冲，**只在本次回调内有效** ——
     * 要留着用请自己拷一份。
     *
     * group     组号（就是编解码单元）—— 通常你会按组换解码器。
     * frame_index 组内第几帧（从组头声明的 frame_start 起数）。
     * ts_delta   **相对本组第一帧**的差，单位是该 track 的 timescale（不是
     *            秒、也不是微秒）—— 具体尺度见 on_track_info。每组的累加基线
     *            会重置，所以它是「组内相对时间」，不是绝对时间。
     * payload    载荷**原样**，一个字节都没动。
     *
     * ⚠️ **载荷前面可能有、也可能没有容器时间戳前缀** —— 库里不替你猜：
     *   - legacy 容器的**媒体**帧（`container.kind == "legacy"`）在载荷最前面
     *     垫一个**绝对微秒**的 varint（实测视频 `80 99 88 b0` = 10062000 us），
     *     剥掉之后才是 AVCC / 裸 AAC；
     *   - **catalog 这类数据轨没有这一层**（JSON 从第 0 字节起）。
     * 剥错的后果是**静默少几个字节**（catalog 的 `{"` 里 `0x7B` 正好是个合法的
     * 2 字节 varint 首字节，剥掉就少两个字符，看着像「JSON 格式怪」），所以
     * 判断权交给订阅方 —— 用 moq_lite_strip_container_ts 剥，它同时把
     * 微秒时间戳解出来。
     *
     * 与媒体层其它地方的 Media_Packet 也不同（那个含 FLV tag 头，消费方必须
     * 剥 5/2 字节）—— 这里给的是「moq 线上原样」。 */
    void (*on_frame)(moq_lite_sub_t sub, uint64_t group, uint64_t frame_index,
                     int64_t ts_delta, const uint8_t *payload, size_t len,
                     void *user);

    /* TRACK 流（bidi 0x6）上的 TRACK_INFO —— 只有**收到对端订阅我们**时才有，
     * 我们当订阅者时对端不发这条（-06 上它是发布端的应答）。留着是因为
     * timescale 只能从这里拿到。 */
    void (*on_track_info)(moq_lite_sub_t sub, uint8_t priority,
                          uint64_t max_age_ms, uint64_t timescale, void *user);

    /* 订阅正常收尾（SUBSCRIBE_END）。end_group 是**排他**上界。 */
    void (*on_subscribe_end)(moq_lite_sub_t sub, uint64_t end_group, void *user);

    /* 对端声明丢了若干组（SUBSCRIBE_DROP）。注意 gstart..gend 是**含**两端的
     * （与 SUBSCRIBE 的 Group End 语义不同）。 */
    void (*on_drop)(moq_lite_sub_t sub, uint64_t gstart, uint64_t gend,
                    uint64_t error_code, void *user);

    void *user;
} moq_lite_sub_cb_t;

/* ── 会话 ─────────────────────────────────── */

/* 建会话（不连接）。返回 0 号 = 失败（OOM / loop 为空）。
 * cb 会被深拷贝，调用方不必保持它存活；user 原样回传。
 *
 * ⚠️ **loop 由调用方提供** —— 本库不拥有事件循环，也不替你跑它。所有回调
 * 都在你传进来的这条 loop 上触发，收尾由你自己 `uv_run(loop, UV_RUN_DEFAULT)`
 * （或你惯用的 UV_RUN_ONCE / NOWAIT —— 因为这条 loop 上很可能还有你的定时器、
 * 信号、别的连接，本库无从知道该跑到什么时候算完）。
 * 传进来的 loop 必须比会话活得久：会话关闭之前不要 uv_loop_close 它。 */
moq_lite_session_t moq_lite_session_new(uv_loop_t *loop);

/* 覆盖要 offer 的 ALPN 列表（进程内全局，**须在 first connect 之前**调）。
 * protos 为 C 字符串数组；count<=0 = 恢复默认 {MOQ_LITE_VERSION}。
 *
 * 为什么默认不 offer h3：本库不会 WebTransport。见头文件顶部。
 * 只有在明确知道对端选得到一个你实现的版本时才加 —— offer 了却不会说那个
 * 协议，症状是「连上了但没有任何下文」。 */
int moq_lite_session_set_alpn(const char *const *protos, int count);

/* 连接。url 形如 `moql://host:port/path`。
 *   - 不写端口用 MOQ_LITE_DEFAULT_PORT；
 *   - 不写路径 = 根；
 *   - 方案只接受 moql:// / moq-lite://（裸 QUIC）。给了 https:// 会返回
 *     MOQ_LITE_ERR_URL —— 那是 WebTransport 那条路，本库不做。
 * ⚠️ host **用域名**（如 www.webrtcserver.com.cn），不要用 127.0.0.1：
 * 服务端证书没有 IP SAN，rustls 之外的严格实现会以 NotValidForName 拒掉。
 * （本库的 TLS 验证是 SSL_VERIFY_NONE，所以 IP 也能连上 —— 但换成任何
 * 做验证的实现就不行了，别养成习惯。）
 * 返回 0 成功入队（结果走 on_connected），<0 立即失败见错误码。 */
int moq_lite_session_connect(moq_lite_session_t s, const char *url,
                             const moq_lite_session_cb_t *cb);

/* 协商到的 ALPN（握手完成后有效）。空串 = 还没握手 / 对端没提供。 */
const char *moq_lite_session_alpn(moq_lite_session_t s);

/* 发 ANNOUNCE_REQUEST 去发现 prefix 之下有哪些广播。
 * prefix 是**绝对**路径；空串 = 「本 scope 下的全部」，这是最常见的用法
 * （也是唯一实测过能拿到结果的那种）。结果走 on_announce；
 * **空集合是合法结果**（对端这个前缀下确实没有东西），不是错误。
 * 返回 0 已发出，<0 失败。 */
int moq_lite_session_announce(moq_lite_session_t s, const char *prefix);

/* 关闭会话（任意线程）。之后所有句柄失效；资源在 loop 线程释放，
 * 无需再调 _free（调了也无害，幂等）。 */
void moq_lite_session_close(moq_lite_session_t s);


/* ── 角色：本端是把媒体推给对端，还是从对端拉 ── */

typedef enum {
    /* 又推又拉。对端发 ANNOUNCE_REQUEST 我们就应答，同时我们自己也去发现。
     * ⚠️ 只有在**确实会应答**时才用：对订阅方发 ANNOUNCE_REQUEST 却不应答
     * 对端的请求，会让对端挂在一条永远等不到回音的流上。 */
    MOQ_LITE_ROLE_BOTH       = 0,
    MOQ_LITE_ROLE_PUBLISHER  = 1,   /* 只推：对端来订阅我们 */
    MOQ_LITE_ROLE_SUBSCRIBER = 2,   /* 只拉（默认）*/
} moq_lite_role_t;

/* 设本端角色（须在 connect 之前）。这个值写进 SETUP 的 Role 参数，对端据此
 * 决定往哪个方向走 —— 拨 moqlivecast 推流时必须设 PUBLISHER，否则服务端只当
 * 我们是订阅方，一个 ANNOUNCE_REQUEST 都不会发过来。 */
int moq_lite_session_set_role(moq_lite_session_t s, moq_lite_role_t role);

/* ── 发布（推流）───────────────────────────────
 *
 * 用法：声明广播 → 声明若干轨（同时写进 catalog）→ 逐帧写 → 收尾。
 * 「谁来订阅、订阅了几条、每组怎么切」全部由库处理，调用方只管喂帧：
 *
 *   ① moq_lite_publish()        声明一条对外广播
 *   ② moq_lite_publish_track()  声明轨（顺带把它写进 catalog）
 *   ③ moq_lite_track_frame()    按时间戳逐帧写
 *   ④ moq_lite_track_end()      收尾
 *
 * 线程：与 `_subscribe` 一样**必须在 loop 线程调**（通常从 uv_timer 里）。 */

/* 一条轨的编解码参数，也决定它在 catalog 里的 rendition 条目。
 *
 * codec 串下游不认识就解不开 —— 视频必须 "avc1." 开头、音频 "mp4a.40."，
 * 例如 "avc1.42c00d" / "mp4a.40.2"。
 * config 是**原字节**的 avcC / AudioSpecificConfig（不是十六进制串、也不是
 * base64 —— catalog 的 description 字段用十六进制，库自己转）。 */
typedef struct {
    const char    *name;         /* 轨名，同时是 catalog rendition 的 key */
    const char    *codec;        /* "avc1.42c00d" / "mp4a.40.2" */
    const uint8_t *config;       /* avcC / AudioSpecificConfig 原字节 */
    size_t         config_len;
    uint32_t       width, height;          /* 视频 */
    uint32_t       sample_rate, channels;  /* 音频 */
    uint32_t       bitrate;      /* 可 0（不上报）*/
} moq_lite_rendition_t;

/* 声明一条我们对外发布的广播（path 即线上广播名，如 "cli_test.hang"）。
 * ⚠️ 必须在 connect **之后** —— 连接没起来就没有对端可公告。
 * 返回 0 = 失败（会话没连上 / OOM）。 */
moq_lite_broadcast_t moq_lite_publish(moq_lite_session_t s, const char *path);

/* 在广播上声明一条轨（同时进 catalog）。is_video 决定它进 video 还是 audio 节。
 * 返回 0 = 失败。至少要声明一条轨，广播才会被公告出去 —— 没有 catalog 的广播
 * 对端不会被通知，也解不开（服务端的 ResolveServeTrack 就是这么卡的）。 */
moq_lite_track_t moq_lite_publish_track(moq_lite_broadcast_t bc, int is_video,
                                        const moq_lite_rendition_t *r);

/* 写一帧。ts_us = **绝对微秒**时间戳。
 *
 * key：视频**必须**在关键帧处开新组（key=1），组内其余帧 key=0；库在「还没开
 * 组」时会丢弃非关键帧 —— 没有关键帧的组解不出来，发出去也是废的。
 * 音频不用管组（传 0 即可），库按 1 秒一组自己切。
 *
 * payload：视频是 AVCC 长度前缀的 NALU，音频是裸 AAC。**不含** FLV tag 头，
 * 也不含容器时间戳前缀 —— 那些由库加。 */
int moq_lite_track_frame(moq_lite_track_t t, uint64_t ts_us,
                         const uint8_t *payload, size_t len, int key);

/* 收尾：给每条订阅当前开着的组补结束哨兵 + FIN，并标记该轨结束。 */
void moq_lite_track_end(moq_lite_track_t t);

/* ── 广播 / 订阅 ────────────────────────────── */

/* 订阅某条 track。callbacks 会被深拷贝。
 *
 * 订阅 `"catalog.json"` 拿到的就是 catalog 的原始 JSON（一帧一批），
 * 库里**不解析**它 —— 那是应用的事（本 API 不假设 catalog 的 schema）。
 *
 * 返回订阅句柄，0 号 = 失败（会话已死 / 广播失效 / OOM）。
 * ⚠️ 必须从 on_announce 回调（或其后）调；广播句柄在 on_announce_end /
 * on_close 之后立即失效。 */
moq_lite_sub_t moq_lite_subscribe(moq_lite_broadcast_t bc,
                                  const char *track,
                                  const moq_lite_sub_cb_t *cb);

/* 取消订阅（关掉那条 bidi 流）。幂等。 */
void moq_lite_unsubscribe(moq_lite_sub_t sub);

/* ── 错误码 ─────────────────────────────────── */

#define MOQ_LITE_OK              0
#define MOQ_LITE_ERR_ARGS       -1   /* 句柄无效 / 参数为空 */
#define MOQ_LITE_ERR_URL        -2   /* URL 解不了 / 方案不是裸 QUIC */
#define MOQ_LITE_ERR_NOMEM      -3
#define MOQ_LITE_ERR_STATE      -4   /* 会话不在能接受该动作的状态 */
#define MOQ_LITE_ERR_INTERNAL   -5

/* 最近一次错误的可读描述（线程局部，随时可读，不必先有失败）。 */
const char *moq_lite_strerror(int err);

/* ── 载荷助手 ───────────────────────────────── */

/* 剥掉 legacy 容器垫在媒体帧最前面的**绝对微秒** varint 时间戳。
 *
 * 成功：ts_us 出参拿到微秒时间戳（可为 NULL），out/out_len 指向前缀之后的
 * 编解码数据（AVCC / 裸 AAC）。返回 MOQ_LITE_OK。
 * 失败（载荷为空 / 开头不是一个完整 varint）：返回 <0，out/out_len 不动 ——
 * 这时**原样使用 payload** 就对了（说明这条轨本来就没有前缀）。
 *
 * ⚠️ **只对媒体轨用**。catalog 这类数据轨没有前缀，硬剥会静默少 2 个字节
 * （`{"` 的 0x7B 是合法的 2 字节 varint 首字节）。见 on_frame 的说明。 */
int moq_lite_strip_container_ts(const uint8_t *payload, size_t len,
                                uint64_t *ts_us, const uint8_t **out,
                                size_t *out_len);

/* ── 事件循环 ───────────────────────────────── */

/* 本库不提供 loop 的驱动接口 —— 循环是你的（见 moq_lite_session_new 的说明）。
 * 在回调里收尾就用你惯用的方式：`uv_stop(loop)`，或者干脆
 * `moq_lite_session_close(s)` 之后让 uv_run 自然退出。 */

#ifdef __cplusplus
}
#endif

#endif /* MOQ_LITE_CLIENT_H */
