#ifndef MOQ_LITE_SERVER_HPP
#define MOQ_LITE_SERVER_HPP

#include "moq/moq_lite_session.hpp"

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
class MoqLiteServer {
public:
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
    std::map<QuicConnection *, std::unique_ptr<MoqLiteSession>> sessions_;
};

} /* namespace cpp_streamer */

#endif /* MOQ_LITE_SERVER_HPP */
