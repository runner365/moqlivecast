#ifndef MOQLIVECAST_FLV_FILE_READER_HPP
#define MOQLIVECAST_FLV_FILE_READER_HPP

#include "flv_demux.hpp"

#include <cstdint>
#include <cstdio>
#include <deque>
#include <memory>
#include <string>
#include <vector>

namespace cpp_streamer {

/* 从 FLV 文件解出的一个裸编码帧。
 *
 * 复用 FlvDemuxer 的 support_flv_media_hdr=true 模式：
 *   - 视频 payload 是 AVCC / HEVC 长度前缀帧；
 *   - 音频 payload 是 raw AAC；
 *   - sequence header 的 payload 是 avcC / hvcC / ASC。
 *
 * 这里不处理 realtime/loop；这些由 FlvFileSource 负责。 */
struct FlvFileFrame {
    bool is_video = false;
    bool is_h265 = false;
    bool is_seq_hdr = false;
    bool key = false;
    int64_t dts = 0;
    int64_t cts = 0;
    std::vector<uint8_t> payload;
};

class FlvFileReader {
public:
    explicit FlvFileReader(Logger* logger = nullptr);
    ~FlvFileReader();

    FlvFileReader(const FlvFileReader&) = delete;
    FlvFileReader& operator=(const FlvFileReader&) = delete;

    int Open(const std::string& path);
    void Close();

    /* 队列空时继续读；返回 false 表示文件结束或读取失败。 */
    bool Fill();

    bool Empty() const { return queue_.empty(); }
    const FlvFileFrame& Front() const { return queue_.front(); }
    FlvFileFrame TakeFront() {
        FlvFileFrame f = std::move(queue_.front());
        queue_.pop_front();
        return f;
    }

    bool Eof() const { return eof_; }
    const std::string& path() const { return path_; }
    int64_t last_dts() const { return last_dts_; }

private:
    struct Sink;

    std::string path_;
    FILE* fp_ = nullptr;
    Logger* logger_ = nullptr;

    /* demux_ 先声明，sink_ 后声明；析构时 sink 先销毁。 */
    std::unique_ptr<FlvDemuxer> demux_;
    std::unique_ptr<Sink> sink_;

    std::deque<FlvFileFrame> queue_;
    bool eof_ = false;
    int64_t last_dts_ = 0;
    size_t dropped_ = 0;
};

} /* namespace cpp_streamer */

#endif /* MOQLIVECAST_FLV_FILE_READER_HPP */
