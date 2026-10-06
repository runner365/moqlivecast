#include "config/config.hpp"
#include "wt_server/wt_server.hpp"
#include "moq/moq_lite_server.hpp"
#include "moq_handler.hpp"
#include "httpflv_server.hpp"
#include "media_stream_manager.hpp"
#include "utils/logger.hpp"
#include "utils/meta_log.hpp"
#include "utils/timer.hpp"
#include "logger.h"

#include <cstdio>
#include <csignal>
#include <memory>
#include <uv.h>

namespace {

std::unique_ptr<cpp_streamer::WTServer> g_srv;
std::unique_ptr<cpp_streamer::MoqLiteServer> g_moq_lite;
std::unique_ptr<cpp_streamer::Logger> g_cxx_log;
std::unique_ptr<cpp_streamer::HttpFlvServer> g_httpflv;
uv_signal_t g_sig;

void OnSignal(uv_signal_t *sig, int signum) {
    LOG_WARN("[moq] signal %d, shutting down", signum);
    if (g_srv) g_srv->Stop();
    g_srv.reset();
    g_moq_lite.reset();
    g_httpflv.reset();
    cpp_streamer::MetaLog::Instance().Shutdown();
    uv_stop(sig->loop);
}

} /* namespace */

int main(int argc, char *argv[]) {
    const char *cfg_path = "moqlivecast/etc/config.yml";
    if (argc != 2) {
        printf("usage: %s [config.yml]\n", argv[0]);
        return 1;
    }
    cfg_path = argv[1];

    setbuf(stdout, nullptr);

    auto &cfg = cpp_streamer::Config::Instance();
    if (!cfg.Load(cfg_path)) {
        return 1;
    }
    log_set_level(cfg.LogLevelValue());
    log_set_console(cfg.LogConsole() ? 1 : 0);
    if (!cfg.LogFilename().empty()) {
        log_set_file(cfg.LogFilename().c_str());
    }

    uv_loop_t *loop = uv_default_loop();
    uv_signal_init(loop, &g_sig);
    uv_signal_start(&g_sig, OnSignal, SIGINT);

    cpp_streamer::StreamerTimerInitialize(loop, 100);
    cpp_streamer::MetaLog::Instance().Init(cfg.MetaLogFile(), cfg.MetaLogInterval());

    cpp_streamer::LOGGER_LEVEL cxx_level = cpp_streamer::LOGGER_INFO_LEVEL;
    if (cfg.LogLevelValue() == DEBUG) cxx_level = cpp_streamer::LOGGER_DEBUG_LEVEL;
    else if (cfg.LogLevelValue() == WARN) cxx_level = cpp_streamer::LOGGER_WARN_LEVEL;
    else if (cfg.LogLevelValue() == ERROR) cxx_level = cpp_streamer::LOGGER_ERROR_LEVEL;
    g_cxx_log = std::make_unique<cpp_streamer::Logger>(cfg.LogFilename(), cxx_level);
    cpp_streamer::MediaStreamManager::SetLogger(g_cxx_log.get());

    g_httpflv = std::make_unique<cpp_streamer::HttpFlvServer>(
        loop, cfg.HttpFlvListenIp(),
        static_cast<uint16_t>(cfg.HttpFlvPort()),
        cfg.KeyFile(), cfg.CertFile(), g_cxx_log.get());
    LOG_INFO("[moq] http-flv https://%s:%d/{app}/{stream}.flv",
             cfg.HttpFlvListenIp().c_str(), cfg.HttpFlvPort());

    g_srv = std::make_unique<cpp_streamer::WTServer>(loop);
    if (!g_srv->Handle()) {
        LOG_ERROR("[moq] WTServer create failed");
        return 1;
    }

    for (const auto &sp : cfg.SubPaths()) {
        if (g_srv->AddPath(sp.path,
                           cpp_streamer::MoqHandler::OnSession,
                           cpp_streamer::MoqHandler::OnStreamData,
                           cpp_streamer::MoqHandler::OnSessionClose) < 0) {
            LOG_ERROR("[moq] AddPath failed: %s (%s)",
                      sp.path.c_str(), sp.desc.c_str());
            g_srv.reset();
            return 1;
        }
        LOG_INFO("[moq] add path %s desc=%s",
                 sp.path.c_str(), sp.desc.c_str());
    }

    {
        /* 裸 QUIC 的 moq-lite（binding 1）与 WebTransport 共用这一个 UDP 端口，
         * 靠握手时的 ALPN 分流。ALPN 表来自配置：
         *   h3            → 浏览器 WebTransport
         *   moq-lite-06   → OBS / libmoq 的裸 QUIC
         * 分派点在 http3_server 的握手完成回调里，这里只负责把接住的那半边
         * 接上。 */
        g_moq_lite = std::make_unique<cpp_streamer::MoqLiteServer>();
        /* 用独立的路由表（config 的 moq_lite_subpath），不复用 WebTransport
         * 那张 —— 两者的 path 语义不同，见 config.hpp 的说明。 */
        for (const auto &sp : cfg.MoqLiteSubPaths()) {
            g_moq_lite->AddPath(
                sp.path,
                /* ── 会话起来了，只记账 ──
                 * 协商（announce → catalog → 各条 track 订阅）由 MoqLiteSession
                 * 自己在路由定下来之后往下走，这里不用推它。M3 的落点是
                 * 会话侧解出的帧 → MediaStreamManager::WriterMediaPacket。 */
                [](cpp_streamer::MoqLiteSession &sess,
                   const std::string &path) {
                    LOG_INFO("[moq] moq-lite session up: %s:%d path=%s",
                             sess.RemoteIp().c_str(), sess.RemotePort(),
                             path.c_str());
                },
                [](cpp_streamer::MoqLiteSession &sess) {
                    /* 收尾摘要 —— 「announce 回了空集合」和「catalog 里没有
                     * rendition」在这里一眼可分，不必去翻中间的日志。
                     * delivered 是 M3 的验收数字：它不为 0 才说明帧真的进了
                     * MediaStreamManager（frames 只是收了多少，可能全被
                     * codec 不认 / owner 被顶 / 流太短挡掉了）。 */
                    LOG_INFO("[moq] moq-lite session down: %s:%d path=%s "
                             "broadcast=\"%s\" stream_key=\"%s\" tracks=%zu",
                             sess.RemoteIp().c_str(), sess.RemotePort(),
                             sess.RoutedPath().c_str(),
                             sess.BroadcastPath().c_str(),
                             sess.StreamKey().c_str(),
                             sess.Tracks().size());
                    for (const auto &kv : sess.Tracks()) {
                        const cpp_streamer::MoqLiteTrack &t = kv.second;
                        LOG_INFO("[moq]   track \"%s\" kind=%s codec=%s "
                                 "subscribe_id=%llu started=%d groups=%llu "
                                 "frames=%llu delivered=%llu",
                                 kv.first.c_str(), t.kind.c_str(),
                                 t.codec.c_str(),
                                 static_cast<unsigned long long>(t.subscribe_id),
                                 t.started ? 1 : 0,
                                 static_cast<unsigned long long>(t.groups),
                                 static_cast<unsigned long long>(t.frames),
                                 static_cast<unsigned long long>(t.delivered));
                    }
                });
            LOG_INFO("[moq] moq-lite add path %s desc=%s",
                     sp.path.c_str(), sp.desc.c_str());
        }
        g_srv->SetOnOtherAlpn(
            [](QuicConnection *conn, const std::string &alpn,
               const std::string &remote_ip, int remote_port) {
                if (g_moq_lite) {
                    g_moq_lite->OnConnection(conn, alpn, remote_ip, remote_port);
                } else {
                    QuicConnectionClose(conn, 0x0, "no moq-lite handler");
                }
            });

        const std::vector<std::string> &alpn = cfg.AlpnList();
        std::vector<const char *> alpn_ptrs;
        alpn_ptrs.reserve(alpn.size());
        for (const auto &a : alpn) alpn_ptrs.push_back(a.c_str());

        if (!g_srv->Start(cfg.CertFile().c_str(), cfg.KeyFile().c_str(),
                          cfg.ListenIp().c_str(), cfg.Port(),
                          alpn_ptrs.data(), (int)alpn_ptrs.size())) {
            LOG_ERROR("[moq] listen failed");
            g_srv.reset();
            return 1;
        }
    }
    LOG_INFO("[moq] listening on %s:%d paths=%zu alpn=%zu",
             cfg.ListenIp().c_str(), cfg.Port(), cfg.SubPaths().size(),
             cfg.AlpnList().size());

    uv_run(loop, UV_RUN_DEFAULT);

    g_srv.reset();
    g_httpflv.reset();
    cpp_streamer::MetaLog::Instance().Shutdown();
    g_cxx_log.reset();
    log_shutdown();
    return 0;
}
