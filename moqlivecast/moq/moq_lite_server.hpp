#ifndef MOQ_LITE_SERVER_HPP
#define MOQ_LITE_SERVER_HPP

#include "moq/moq_lite_session.hpp"
#include "media_stream_manager.hpp"
#include "utils/timer.hpp"

#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <string>

namespace cpp_streamer {

/* 裸 QUIC 上的 moq-lite 服务端（binding 1）。
 *
 * 它不是一个监听器 —— 端口和 TLS 都归 WTServer 那个 QuicListener。本类只接
 * `WTServer::SetOnOtherAlpn` 的交割：握手协商出 moq-lite 的 ALPN 时，
 * http3_server 会把连接交出来，这里按 ALPN 认领并建会话。
 *
 * 会话生命周期与 WebTransport 那边不同：WT 的 session 在 CONNECT 之后才有，
 * 而 moq-lite 的会话在握手完成时就成立 —— SETUP 是会话的第一件事，
 * 不是它的前提。 */
class MoqLiteServer : public TimerInterface, public StreamManagerCallbackI {
public:
    MoqLiteServer();
    ~MoqLiteServer() override;
    /* 对端 SETUP 到齐、路径认得、会话正式起来了。
     * path 是归一化后的路由路径（不带前导 '/'）。 */
    using OnSession = std::function<void(MoqLiteSession &sess,
                                         const std::string &path)>;
    using OnSessionClose = std::function<void(MoqLiteSession &sess)>;

    /* 注册可路由的路径。写法与 WebTransport 的 subpath 一致（"moq"，不带
     * 前导 '/'）；收到 SETUP 的 Path 会先归一化再比，所以 "/moq" 等效。
     * 多条时第一条即「服务端默认路径」，对端不带 Path 参数时用它。 */
    void AddPath(const std::string &path, OnSession on_session,
                 OnSessionClose on_close = nullptr);

    /* 这个 ALPN 归我们吗。判断用前缀 `moq-lite-`，不写死版本号 ——
     * 具体协商到哪版由 TLS 的 ALPN 表决定（那份表在 config 里）。 */
    static bool Handles(const std::string &alpn);

    /* 接 WTServer::SetOnOtherAlpn。alpn 不归我们时立刻关掉连接。 */
    void OnConnection(QuicConnection *conn, const std::string &alpn,
                      const std::string &remote_ip, int remote_port);

    size_t SessionCount() const { return sessions_.size(); }
    /* 进程退出/停服时主动收摊。不断开连接，只丢本地会话。 */
    void Clear();

    /* catalog.json 更新：转发给每个会话，由会话给它的 catalog 订阅方补发新组。
     * 必须在唯一 uv loop 线程调用（MediaStreamManager 的回调就是这个线程）。 */
    void OnCatalogUpdated(const std::string &stream_key, uint64_t rev);

    /* 本地缺流钩子（集群 edge）：返回 true = 已发起回源，SUBSCRIBE 可挂起
     * timeout_ms 等 catalog。未设置时行为与之前一致（立即 reset）。 */
    using StreamMissingFn =
        std::function<bool(const std::string &local_key, const std::string &wire)>;
    void SetOnStreamMissing(StreamMissingFn fn, int timeout_ms);

    /* 集群 edge：额外的可公告广播集合（Center 公告过来的）。 */
    using RemoteAnnounceProvider =
        std::function<void(std::vector<std::string> *out)>;
    void SetRemoteAnnounceProvider(RemoteAnnounceProvider fn);
    /* Center 的远端广播集合发生变化：向本地订阅方的 announce 流补发
     * ANNOUNCE_START / ANNOUNCE_END。 */
    void NotifyRemoteAnnounce(const std::string &wire, bool added);

    bool OnTimer() override;

    /* StreamManagerCallbackI：本地上推消失 → 向订阅方发 ANNOUNCE_END。 */
    void OnPublish(const std::string &app,
                   const std::string &streamname) override;
    void OnUnpublish(const std::string &app,
                     const std::string &streamname) override;

private:
    struct PathCb {
        OnSession     on_session;
        OnSessionClose on_close;
    };

    /* moq-lite 会话的回调：解出 Path 后由 MoqLiteSession 调进来。
     * 返回 false = 不认这个路径，会话会被关掉（-06 §7.3.2）。 */
    bool RouteTo(MoqLiteSession &sess);
    void RemoveSession(MoqLiteSession &sess);

    std::map<std::string, PathCb> paths_;
    /* 「第一条」= 首次 AddPath 的那条。不能拿 paths_.begin() 代替 —— 那是
     * **字典序**最小的，配置写 flv/moq 时它会选 flv，与上面 AddPath 的承诺不符。 */
    std::string default_path_;
    /* shared_ptr 而不是 unique_ptr：catalog 更新回调要遍历会话并调用它，
     * 而回调里的 QUIC 写可能间接触发会话关闭（RemoveSession → erase）。
     * 遍历前复制一份 shared_ptr 快照，能保证被摘掉的会话在本轮调用期间
     * 对象仍然存活，不会在回调中途变成野指针。 */
    std::map<QuicConnection *, std::shared_ptr<MoqLiteSession>> sessions_;

    StreamMissingFn on_stream_missing_;
    RemoteAnnounceProvider remote_announce_provider_;
    int deferred_subscribe_ms_ = 3000;
};

} /* namespace cpp_streamer */

#endif /* MOQ_LITE_SERVER_HPP */
