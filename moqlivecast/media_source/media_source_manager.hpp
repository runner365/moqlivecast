#ifndef MOQLIVECAST_MEDIA_SOURCE_MANAGER_HPP
#define MOQLIVECAST_MEDIA_SOURCE_MANAGER_HPP

#include "config/config.hpp"
#include "media_source_sink.hpp"
#include "flv_file_source.hpp"

#include <memory>
#include <string>
#include <vector>

#include <uv.h>

namespace cpp_streamer {

/* 按 YAML media_sources 创建、启动和停止本地媒体源。
 *
 * 生命周期：
 *   Start() -> source thread -> sink uv_async -> uv loop
 *   Stop()  -> source stop/join -> sink release publisher
 */
class MediaSourceManager {
public:
    explicit MediaSourceManager(uv_loop_t* loop);
    ~MediaSourceManager();

    MediaSourceManager(const MediaSourceManager&) = delete;
    MediaSourceManager& operator=(const MediaSourceManager&) = delete;

    /* 在 uv loop 线程调用。单个 source 启动失败只记录错误，不阻止其他 source。 */
    void Start(const std::vector<MediaSourceConfig>& configs);
    /* 在 uv loop 线程调用；幂等。 */
    void Stop();

private:
    struct Entry {
        std::string id;
        std::unique_ptr<MediaSourceSink> sink;
        std::unique_ptr<FlvFileSource> source;
    };

    uv_loop_t* loop_ = nullptr;
    std::vector<Entry> entries_;
    bool started_ = false;
};

} /* namespace cpp_streamer */

#endif /* MOQLIVECAST_MEDIA_SOURCE_MANAGER_HPP */
