#ifndef MOQLIVECAST_MEDIA_SOURCE_SINK_HPP
#define MOQLIVECAST_MEDIA_SOURCE_SINK_HPP

#include "media_source.hpp"
#include "flv_catalog.hpp"

#include <atomic>
#include <deque>
#include <mutex>
#include <string>

#include <uv.h>

namespace cpp_streamer {

/* source 事件 -> uv loop -> MediaStreamManager。
 *
 * 这是 IMediaSink 的服务端实现：
 *   - source 线程调用 onTrackInfo/onVideo/onAudio 时只入队；
 *   - uv_async_send() 唤醒 uv loop；
 *   - Drain() 在 uv loop 里封装 FLV body 并写 MediaStreamManager。
 */
class MediaSourceSink : public IMediaSink {
public:
    MediaSourceSink(uv_loop_t* loop,
                    std::string app,
                    std::string stream,
                    std::string source_id);
    ~MediaSourceSink() override;

    MediaSourceSink(const MediaSourceSink&) = delete;
    MediaSourceSink& operator=(const MediaSourceSink&) = delete;

    /* 在 uv loop 线程调用：ClaimPublisher + 初始化 sink。 */
    int Start();
    /* 在 uv loop 线程调用：释放 publisher 并停止接收事件。 */
    void Stop();

    int onTrackInfo(const TrackInfo& info) override;
    int onVideo(MediaFrame frame) override;
    int onAudio(MediaFrame frame) override;
    void onEof() override;
    void onError(int code, const std::string& message) override;
    int onMetadata(const uint8_t* data, size_t len) override;

private:
    enum class EventType {
        TrackInfo,
        Video,
        Audio,
        Eof,
        Error,
    };

    struct Event {
        EventType type = EventType::Error;
        TrackInfo info;
        MediaFrame frame;
        int code = 0;
        std::string message;
    };

    static void OnAsync(uv_async_t* handle);
    int Enqueue(Event&& event, bool allow_backpressure);
    void Drain();
    void HandleTrackInfo(const TrackInfo& info);
    void HandleVideo(const MediaFrame& frame);
    void HandleAudio(const MediaFrame& frame);
    void PublishCatalog();
    void WriteSequenceHeader(const TrackInfo& info);
    void Finish();

    uv_loop_t* loop_ = nullptr;
    uv_async_t async_;
    bool async_ready_ = false;
    std::atomic<bool> active_{false};
    std::atomic<bool> started_{false};

    std::string app_;
    std::string stream_;
    std::string source_id_;
    std::string stream_key_;
    std::string owner_id_;

    std::mutex queue_mutex_;
    std::deque<Event> queue_;
    static constexpr size_t kMaxQueuedFrames = 1024;

    FlvCatalogBuilder catalog_;
    std::string last_catalog_json_;
    bool logged_first_video_ = false;
    bool logged_first_audio_ = false;
    uint64_t video_frames_ = 0;
    uint64_t audio_frames_ = 0;
};

} /* namespace cpp_streamer */

#endif /* MOQLIVECAST_MEDIA_SOURCE_SINK_HPP */
