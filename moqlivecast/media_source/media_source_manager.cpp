#include "media_source_manager.hpp"

#include "logger.h"

namespace cpp_streamer {

MediaSourceManager::MediaSourceManager(uv_loop_t* loop) : loop_(loop) {}

MediaSourceManager::~MediaSourceManager() {
    Stop();
}

void MediaSourceManager::Start(const std::vector<MediaSourceConfig>& configs) {
    if (started_) {
        LOG_WARN("[media-source] manager already started");
        return;
    }
    started_ = true;

    if (configs.empty()) {
        LOG_INFO("[media-source] no media_sources configured");
        return;
    }

    entries_.reserve(configs.size());
    size_t disabled = 0;
    for (const auto& cfg : configs) {
        if (!cfg.enable) {
            ++disabled;
            LOG_INFO("[media-source] skip disabled source id=%s app=%s stream=%s",
                     cfg.id.c_str(), cfg.app.c_str(), cfg.stream.c_str());
            continue;
        }
        Entry entry;
        entry.id = cfg.id;
        entry.sink.reset(new MediaSourceSink(loop_, cfg.app, cfg.stream, cfg.id));
        if (entry.sink->Start() < 0) {
            LOG_ERROR("[media-source] %s sink start failed", cfg.id.c_str());
            continue;
        }

        SourceConfig src_cfg;
        src_cfg.source_id = cfg.id;
        src_cfg.paths = cfg.paths;
        src_cfg.realtime = cfg.realtime;
        src_cfg.loop = cfg.loop;

        entry.source.reset(new FlvFileSource());
        if (entry.source->start(cfg.app, cfg.stream, src_cfg, entry.sink.get()) < 0) {
            LOG_ERROR("[media-source] %s source start failed path=%s",
                      cfg.id.c_str(), cfg.paths.empty() ? "" : cfg.paths[0].c_str());
            entry.sink->Stop();
            continue;
        }
        LOG_INFO("[media-source] %s wired app=%s stream=%s path=%s",
                 cfg.id.c_str(), cfg.app.c_str(), cfg.stream.c_str(),
                 cfg.paths[0].c_str());
        entries_.push_back(std::move(entry));
    }
    LOG_INFO("[media-source] manager started sources=%zu disabled=%zu",
             entries_.size(), disabled);
}

void MediaSourceManager::Stop() {
    if (!started_ && entries_.empty()) return;
    started_ = false;

    /* 先停 source（它会 join 读线程），再停 sink（释放 publisher）。 */
    for (auto& entry : entries_) {
        if (entry.source) entry.source->stop();
    }
    for (auto& entry : entries_) {
        if (entry.sink) entry.sink->Stop();
    }
    entries_.clear();
    LOG_INFO("[media-source] manager stopped");
}

} /* namespace cpp_streamer */
