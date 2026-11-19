#include "flv_file_source.hpp"

#include "logger.h"

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <exception>

namespace cpp_streamer {

namespace {

int64_t NowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

constexpr int64_t kLoopGapMs = 40;
constexpr int64_t kMaxSleepSliceMs = 5;
constexpr int64_t kBackpressureWaitMs = 5;

bool CheckFlvFile(const std::string& path, const std::string& source_id) {
    FILE* fp = fopen(path.c_str(), "rb");
    if (!fp) {
        LOG_ERROR("[media-source] source %s cannot open playlist file: %s (%s)",
                  source_id.c_str(), path.c_str(), strerror(errno));
        return false;
    }
    uint8_t head[9] = {0};
    const size_t n = fread(head, 1, sizeof(head), fp);
    fclose(fp);
    if (n < sizeof(head) || head[0] != 'F' || head[1] != 'L' || head[2] != 'V') {
        LOG_ERROR("[media-source] source %s playlist file is not a valid FLV: %s",
                  source_id.c_str(), path.c_str());
        return false;
    }
    return true;
}

} /* namespace */

FlvFileSource::~FlvFileSource() {
    stop();
}

int FlvFileSource::start(const std::string& app_name,
                         const std::string& stream_name,
                         const SourceConfig& config,
                         IMediaSink* sink) {
    if (running_.load()) {
        LOG_WARN("[media-source] source %s already running", config.source_id.c_str());
        return -1;
    }
    if (!sink || config.paths.empty()) {
        LOG_ERROR("[media-source] source %s invalid config: paths=%zu sink=%p",
                  config.source_id.c_str(), config.paths.size(), (void*)sink);
        return -EINVAL;
    }
    for (const auto& path : config.paths) {
        if (path.empty() || !CheckFlvFile(path, config.source_id)) {
            LOG_ERROR("[media-source] source %s invalid playlist entry: %s",
                      config.source_id.c_str(), path.c_str());
            return -ENOENT;
        }
    }

    config_ = config;
    app_ = app_name;
    stream_ = stream_name;
    sink_ = sink;
    stop_ = false;
    first_log_dts_ = -1;

    current_path_index_ = 0;
    reader_.reset(new FlvFileReader(nullptr));
    if (reader_->Open(config_.paths[current_path_index_]) < 0) {
        LOG_ERROR("[media-source] source %s open failed: %s",
                  config_.source_id.c_str(),
                  config_.paths[current_path_index_].c_str());
        reader_.reset();
        return -1;
    }

    running_ = true;
    try {
        thread_ = std::thread(&FlvFileSource::Run, this);
    } catch (const std::exception& e) {
        running_ = false;
        LOG_ERROR("[media-source] source %s thread create failed: %s",
                  config_.source_id.c_str(), e.what());
        reader_.reset();
        return -1;
    }
    LOG_INFO("[media-source] source %s started app=%s stream=%s files=%zu "
             "first=%s realtime=%d loop=%d",
             config_.source_id.c_str(), app_.c_str(), stream_.c_str(),
             config_.paths.size(), config_.paths[0].c_str(),
             config_.realtime ? 1 : 0, config_.loop ? 1 : 0);
    return 0;
}

void FlvFileSource::stop() {
    const bool was_running = running_.exchange(false);
    stop_ = true;
    if (thread_.joinable()) {
        if (thread_.get_id() != std::this_thread::get_id()) {
            thread_.join();
        } else {
            /* 不允许在 source 自己的线程里 join 自己；正常退出路径由 manager
             * 从 uv loop 调用 stop()，不会走到这里。 */
            thread_.detach();
        }
    }
    if (reader_) {
        reader_->Close();
        reader_.reset();
    }
    if (was_running) {
        LOG_INFO("[media-source] source %s stopped",
                 config_.source_id.c_str());
    }
}

bool FlvFileSource::SleepUntil(int64_t due_ms) {
    while (!stop_.load()) {
        const int64_t now = NowMs();
        if (now >= due_ms) return true;
        int64_t left = due_ms - now;
        if (left > kMaxSleepSliceMs) left = kMaxSleepSliceMs;
        std::this_thread::sleep_for(std::chrono::milliseconds(left));
    }
    return false;
}

void FlvFileSource::EmitTrackInfo(const FlvFileFrame& frame) {
    TrackInfo info;
    if (frame.is_video) {
        info.id = MediaTrackId::Video;
        info.codec = frame.is_h265 ? MEDIA_CODEC_H265 : MEDIA_CODEC_H264;
    } else {
        info.id = MediaTrackId::Audio;
        info.codec = MEDIA_CODEC_AAC;
    }
    info.extradata = frame.payload;
    const int ret = sink_->onTrackInfo(info);
    if (ret < 0) {
        LOG_ERROR("[media-source] source %s track info rejected ret=%d",
                  config_.source_id.c_str(), ret);
        sink_->onError(ret, "track info rejected");
        stop_ = true;
    } else {
        LOG_INFO("[media-source] source %s track=%s codec=%d extradata=%zuB",
                 config_.source_id.c_str(),
                 frame.is_video ? "video" : "audio",
                 static_cast<int>(info.codec), info.extradata.size());
    }
}

void FlvFileSource::Run() {
    const int64_t wall_start_ms = NowMs();
    int64_t first_raw_dts = -1;
    int64_t first_out_dts = -1;
    int64_t last_out_dts = -1;
    int64_t loop_base_ms = 0;
    uint64_t loop_count = 0;
    uint64_t video_count = 0;
    uint64_t audio_count = 0;

    LOG_INFO("[media-source] source %s reader thread start",
             config_.source_id.c_str());

    try {
        while (!stop_.load()) {
            if (!reader_ || reader_->Empty()) {
                if (!reader_ || !reader_->Fill()) {
                    if (stop_.load()) break;
                    size_t next_index = current_path_index_ + 1;
                    bool has_next = next_index < config_.paths.size();
                    if (!has_next) {
                        if (!config_.loop) {
                            LOG_INFO("[media-source] source %s playlist EOF "
                                     "video=%llu audio=%llu files=%zu",
                                     config_.source_id.c_str(),
                                     static_cast<unsigned long long>(video_count),
                                     static_cast<unsigned long long>(audio_count),
                                     config_.paths.size());
                            sink_->onEof();
                            break;
                        }
                        next_index = 0; /* loop：最后一个文件回到第一个 */
                    }

                    ++loop_count;
                    reader_->Close();
                    if (reader_->Open(config_.paths[next_index]) < 0) {
                        LOG_ERROR("[media-source] source %s next file open failed: %s",
                                  config_.source_id.c_str(),
                                  config_.paths[next_index].c_str());
                        sink_->onError(-ENOENT, "next playlist file open failed");
                        break;
                    }
                    current_path_index_ = next_index;
                    loop_base_ms = (last_out_dts >= 0)
                                       ? last_out_dts + kLoopGapMs : 0;
                    first_raw_dts = -1;
                    LOG_INFO("[media-source] source %s switch file index=%zu/%zu "
                             "name=%s base=%lld switches=%llu",
                             config_.source_id.c_str(),
                             current_path_index_ + 1, config_.paths.size(),
                             config_.paths[current_path_index_].c_str(),
                             static_cast<long long>(loop_base_ms),
                             static_cast<unsigned long long>(loop_count));
                    continue;
                }
                continue;
            }

            FlvFileFrame frame = reader_->TakeFront();
            if (frame.is_seq_hdr) {
                EmitTrackInfo(frame);
                if (stop_.load()) break;
                continue;
            }

            if (first_raw_dts < 0) first_raw_dts = frame.dts;
            int64_t out_dts = loop_base_ms + (frame.dts - first_raw_dts);
            if (last_out_dts >= 0 && out_dts <= last_out_dts) {
                out_dts = last_out_dts + 1;
            }
            if (out_dts < 0) out_dts = 0;
            int64_t out_pts = out_dts + frame.cts;
            if (out_pts < out_dts) out_pts = out_dts;

            if (config_.realtime) {
                if (first_out_dts < 0) first_out_dts = out_dts;
                const int64_t due =
                    wall_start_ms + (out_dts - first_out_dts);
                if (!SleepUntil(due)) break;
            }

            auto pkt = std::make_shared<Media_Packet>(frame.payload.size() + 32);
            pkt->av_type_ = frame.is_video ? MEDIA_VIDEO_TYPE : MEDIA_AUDIO_TYPE;
            pkt->codec_type_ = frame.is_video
                                   ? (frame.is_h265 ? MEDIA_CODEC_H265 : MEDIA_CODEC_H264)
                                   : MEDIA_CODEC_AAC;
            pkt->fmt_type_ = MEDIA_FORMAT_FLV;
            pkt->nalu_fmt_type_ = NALU_FORMAT_AVCC;
            pkt->dts_ = out_dts;
            pkt->pts_ = out_pts;
            pkt->is_key_frame_ = frame.key;
            pkt->is_seq_hdr_ = false;
            pkt->app_ = app_;
            pkt->streamname_ = stream_;
            pkt->key_ = app_ + "/" + stream_;
            pkt->buffer_ptr_->AppendData(
                reinterpret_cast<const char*>(frame.payload.data()),
                frame.payload.size());

            int ret = 0;
            do {
                ret = frame.is_video ? sink_->onVideo(pkt) : sink_->onAudio(pkt);
                if (ret == -EAGAIN && !stop_.load()) {
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(kBackpressureWaitMs));
                }
            } while (ret == -EAGAIN && !stop_.load());

            if (ret < 0) {
                LOG_ERROR("[media-source] source %s frame submit failed ret=%d "
                          "dts=%lld video=%d",
                          config_.source_id.c_str(), ret,
                          static_cast<long long>(out_dts), frame.is_video ? 1 : 0);
                sink_->onError(ret, "frame submit failed");
                break;
            }

            if (frame.is_video) ++video_count;
            else ++audio_count;
            if (first_log_dts_ < 0) {
                first_log_dts_ = out_dts;
                LOG_INFO("[media-source] source %s first frame video=%d "
                         "dts=%lld pts=%lld cts=%lld key=%d size=%zu",
                         config_.source_id.c_str(), frame.is_video ? 1 : 0,
                         static_cast<long long>(out_dts),
                         static_cast<long long>(out_pts),
                         static_cast<long long>(frame.cts), frame.key ? 1 : 0,
                         frame.payload.size());
            }
            last_out_dts = out_dts;
        }
    } catch (const std::exception& e) {
        LOG_ERROR("[media-source] source %s exception: %s",
                  config_.source_id.c_str(), e.what());
        if (sink_) sink_->onError(-EIO, e.what());
    } catch (...) {
        LOG_ERROR("[media-source] source %s unknown exception",
                  config_.source_id.c_str());
        if (sink_) sink_->onError(-EIO, "unknown exception");
    }

    running_ = false;
    LOG_INFO("[media-source] source %s reader thread exit stop=%d",
             config_.source_id.c_str(), stop_.load() ? 1 : 0);
}

} /* namespace cpp_streamer */
