#ifndef MOQLIVECAST_FLV_FILE_SOURCE_HPP
#define MOQLIVECAST_FLV_FILE_SOURCE_HPP

#include "media_source.hpp"
#include "flv_file_reader.hpp"

#include <atomic>
#include <memory>
#include <string>
#include <thread>

namespace cpp_streamer {

/* 单 FLV 文件 source。
 *
 * - start() 同步打开文件并创建读线程；
 * - 读线程按 FLV dts 做 realtime pacing；
 * - loop=true 时回绕文件，DTS/PTS 使用单调递增的 loop_base；
 * - 裸编码帧通过 IMediaSink 投递给 MediaSourceSink。
 *
 * 第一版只支持一个 paths[0]；paths 数组为后续节目单预留。
 */
class FlvFileSource : public IMediaSource {
public:
    FlvFileSource() = default;
    ~FlvFileSource() override;

    int start(const std::string& app_name,
              const std::string& stream_name,
              const SourceConfig& config,
              IMediaSink* sink) override;
    void stop() override;
    const SourceConfig& config() const override { return config_; }

private:
    void Run();
    bool SleepUntil(int64_t due_ms);
    void EmitTrackInfo(const FlvFileFrame& frame);

    SourceConfig config_;
    std::string app_;
    std::string stream_;
    IMediaSink* sink_ = nullptr;
    std::unique_ptr<FlvFileReader> reader_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_{false};
    size_t current_path_index_ = 0;
    int64_t first_log_dts_ = -1;
};

} /* namespace cpp_streamer */

#endif /* MOQLIVECAST_FLV_FILE_SOURCE_HPP */
