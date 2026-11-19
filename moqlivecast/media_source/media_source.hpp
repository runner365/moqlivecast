#ifndef MOQLIVECAST_MEDIA_SOURCE_HPP
#define MOQLIVECAST_MEDIA_SOURCE_HPP

#include "av.hpp"
#include "media_packet.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace cpp_streamer {

enum class MediaTrackId {
    Video,
    Audio,
};

using MediaCodec = MEDIA_CODEC_TYPE;
using MediaFrame = Media_Packet_Ptr;

/* 一个轨道的编码配置。extradata 是 avcC / hvcC / AudioSpecificConfig。 */
struct TrackInfo {
    MediaTrackId id = MediaTrackId::Video;
    MediaCodec codec = MEDIA_CODEC_UNKNOWN;
    std::vector<uint8_t> extradata;
};

/* source 的启动配置；app/stream 由 IMediaSource::start() 传入。 */
struct SourceConfig {
    std::string source_id;
    std::vector<std::string> paths;
    bool realtime = true;
    bool loop = false;
};

/**
 * source -> server 的回调接口。
 *
 * 这些方法可以从 source 自己的线程调用。实现方（MediaSourceSink）只负责
 * 入队并通过 uv_async_send() 投递到 uv loop；真正的 MediaStreamManager
 * 访问必须发生在 uv loop 线程。
 *
 * 返回值：
 *   0        已接受
 *   -EAGAIN  暂时不能接收；文件源应短暂等待后重试
 *   -EINVAL  参数或状态非法
 *   -ENOTSUP 不支持
 *   其它负值 source 应停止并上报 onError
 */
class IMediaSink {
public:
    virtual ~IMediaSink() = default;

    virtual int onTrackInfo(const TrackInfo& info) = 0;
    virtual int onVideo(MediaFrame frame) = 0;
    virtual int onAudio(MediaFrame frame) = 0;

    /* source 正常结束（非 loop）。 */
    virtual void onEof() = 0;
    /* source 错误；之后 source 进入停止状态。 */
    virtual void onError(int code, const std::string& message) = 0;

    /* 第一版返回 -ENOTSUP。 */
    virtual int onMetadata(const uint8_t* data, size_t len) = 0;
};

class IMediaSource {
public:
    virtual ~IMediaSource() = default;

    /* 只负责启动；失败通过返回值或 onError 上报。 */
    virtual int start(const std::string& app_name,
                      const std::string& stream_name,
                      const SourceConfig& config,
                      IMediaSink* sink) = 0;

    /* 幂等；第一版为同步 stop，返回前 source 自己的线程必须退出。 */
    virtual void stop() = 0;

    virtual const SourceConfig& config() const = 0;
};

} /* namespace cpp_streamer */

#endif /* MOQLIVECAST_MEDIA_SOURCE_HPP */
