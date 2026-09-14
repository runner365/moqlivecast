#include "config/config.hpp"
#include "wt_server/wt_server.hpp"
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
std::unique_ptr<cpp_streamer::Logger> g_cxx_log;
std::unique_ptr<cpp_streamer::HttpFlvServer> g_httpflv;
uv_signal_t g_sig;

void OnSignal(uv_signal_t *sig, int signum) {
    LOG_WARN("[moq] signal %d, shutting down", signum);
    if (g_srv) g_srv->Stop();
    g_srv.reset();
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

    if (!g_srv->Start(cfg.CertFile().c_str(), cfg.KeyFile().c_str(),
                      cfg.ListenIp().c_str(), cfg.Port())) {
        LOG_ERROR("[moq] listen failed");
        g_srv.reset();
        return 1;
    }
    LOG_INFO("[moq] media over webtransport listening on %s:%d paths=%zu",
             cfg.ListenIp().c_str(), cfg.Port(), cfg.SubPaths().size());

    uv_run(loop, UV_RUN_DEFAULT);

    g_srv.reset();
    g_httpflv.reset();
    cpp_streamer::MetaLog::Instance().Shutdown();
    g_cxx_log.reset();
    log_shutdown();
    return 0;
}
