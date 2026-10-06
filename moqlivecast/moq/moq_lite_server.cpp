#include "moq/moq_lite_server.hpp"

#include "logger.h"

#include <cstring>

namespace cpp_streamer {

namespace {

const char *kAlpnPrefix = "moq-lite-";

/* 与 MoqLiteSession 里那份保持一致的路径归一化：去掉一个前导 '/'。
 * 单列一份是为了让「配置写 moq、线上来 /moq」这条规则在两边都看得见。 */
std::string NormalizePath(const std::string &in) {
    if (!in.empty() && in[0] == '/') return in.substr(1);
    return in;
}

} /* namespace */

bool MoqLiteServer::Handles(const std::string &alpn) {
    return alpn.rfind(kAlpnPrefix, 0) == 0;
}

void MoqLiteServer::AddPath(const std::string &path, OnSession on_session,
                            OnSessionClose on_close) {
    PathCb cb;
    cb.on_session = std::move(on_session);
    cb.on_close   = std::move(on_close);
    const std::string key = NormalizePath(path);
    if (default_path_.empty()) default_path_ = key;   /* 第一条即默认路径 */
    paths_[key] = std::move(cb);
}

void MoqLiteServer::OnConnection(QuicConnection *conn, const std::string &alpn,
                                 const std::string &remote_ip, int remote_port) {
    if (!conn) return;

    /* 端口上可能配了别的非 h3 协议（比如 quic-echo）。不归我们的别硬接，
     * 接了就没人处理 —— 直接关掉，让对端立刻知道。
     * 这两条早退路径上还没建会话，on_close 仍是 http3_server 的 h3_on_close，
     * 它会做 listener 记账 —— 所以不必在这里自己补 QuicListenerRemoveConn。 */
    if (!Handles(alpn)) {
        LOG_WARN("[moq-lite] %s:%d alpn=%s is not moq-lite, closing",
                 remote_ip.c_str(), remote_port, alpn.c_str());
        QuicConnectionClose(conn, 0x0, "unsupported alpn");
        return;
    }

    if (paths_.empty()) {
        LOG_ERROR("[moq-lite] %s:%d no path registered, closing",
                  remote_ip.c_str(), remote_port);
        QuicConnectionClose(conn, 0x0, "no path");
        return;
    }

    /* 一条连接只能有一条会话。重复建的话旧的析构会把新的那张注册表项抹掉
     * （析构按 conn 反查），后果是后面所有数据都找不到会话。
     * 这里也必须**关掉连接**：只 return 的话对端会挂在一个没人处理的连接上，
     * 且它的 on_close 还指着 http3_server 的 h3_on_close —— 上面两条早退路径
     * 也是这么做的，理由相同。 */
    if (sessions_.find(conn) != sessions_.end()) {
        LOG_ERROR("[moq-lite] %s:%d connection already has a session, closing",
                  remote_ip.c_str(), remote_port);
        QuicConnectionClose(conn, 0x0, "session exists");
        return;
    }

    auto sess = std::unique_ptr<MoqLiteSession>(new MoqLiteSession(
        conn, alpn, remote_ip, remote_port,
        [this](MoqLiteSession &s) { return RouteTo(s); },
        [this](MoqLiteSession &s) { RemoveSession(s); }));

    MoqLiteSession *raw = sess.get();
    sessions_[conn] = std::move(sess);
    raw->Start();
}

bool MoqLiteServer::RouteTo(MoqLiteSession &sess) {
    std::string path = sess.PathOnly();
    /* -06 §7.3.2：Path 为空等价于没发 —— 二者都请求服务端的默认路径。
     * 没了参数也就没有凭据，这里只认默认路径，不做鉴权（本轮没有鉴权）。
     * default_path_ 是**首次 AddPath** 的那条（见头文件里的说明）。 */
    if (path.empty()) path = default_path_;

    auto it = paths_.find(path);
    if (it == paths_.end()) {
        LOG_WARN("[moq-lite] %s:%d path=\"%s\" not served (known: %zu)",
                 sess.RemoteIp().c_str(), sess.RemotePort(), path.c_str(),
                 paths_.size());
        return false;
    }

    sess.SetRoutedPath(path);
    LOG_INFO("[moq-lite] %s:%d session up: path=\"%s\" query=\"%s\" "
             "sessions=%zu",
             sess.RemoteIp().c_str(), sess.RemotePort(), path.c_str(),
             sess.Query().c_str(), sessions_.size());

    /* 回调拿到的会话在下一次回调前一直有效。它不能把会话从本表里摘掉 ——
     * 真正的回收走 RemoveSession，重复摘会让后到的引用悬空。 */
    if (it->second.on_session) it->second.on_session(sess, path);
    return true;
}

void MoqLiteServer::RemoveSession(MoqLiteSession &sess) {
    /* 先把要用的都取出来 —— erase 之后 sess 就没了 */
    QuicConnection *conn = sess.Conn();
    const std::string path = sess.RoutedPath();
    const std::string ip = sess.RemoteIp();
    const int port = sess.RemotePort();

    auto it = sessions_.find(conn);
    if (it == sessions_.end()) return;

    /* 通知放在 erase 之前：回调里读会话状态是安全的，读完我们才析构。
     * 反过来的话回调拿到的是野引用。 */
    auto pit = paths_.find(path);
    if (pit != paths_.end() && pit->second.on_close) {
        pit->second.on_close(sess);
    }

    sessions_.erase(it);
    LOG_INFO("[moq-lite] %s:%d session removed path=\"%s\", %zu left",
             ip.c_str(), port, path.c_str(), sessions_.size());
}

void MoqLiteServer::Clear() {
    sessions_.clear();
}

} /* namespace cpp_streamer */
