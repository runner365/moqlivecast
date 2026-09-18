#ifndef FLV2MOQ_FLV_READER_HPP
#define FLV2MOQ_FLV_READER_HPP

/* ============================================
 * FLV 文件读取 → 解出 H.264 / AAC 帧
 *
 * 复用服务端的 FlvDemuxer，但必须用 support_flv_media_hdr = true：
 *   true  → 输出「FLV tag body」原文（视频 AVCC、音频含 2B 头）
 *   false → 输出 Annex-B 且按 NALU 拆包（服务端拼不回原样）
 *
 * 服务端 OnLocObject 会把 OBJECT payload 原样当作 FLV tag body 拼回，
 * 所以这里必须保持 FLV 原生格式，只剥掉外层 tag header。
 *
 * 采用增量读取：每次 Fill() 只读一个 chunk 并解包入队，避免把整个
 * 文件（长片可达数百 MB）一次性读进内存。调用方按需 Fill。
 * ============================================ */

#include "flv_demux.hpp"

#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <vector>

namespace flv2moq {

/* 廉价校验：只看文件头 9 字节的 "FLV" magic。
 * 用于在建立连接之前先排除「路径写错/不是 FLV」，避免白连一次。
 * 返回 true 表示看起来是 FLV；失败时 err 给出原因。 */
bool CheckFlvMagic(const std::string &path, std::string &err);

/* 解出的一个帧 */
struct FlvFrame {
    bool is_video = false;
    bool is_seq_hdr = false;   /* 视频 avcC / 音频 ASC */
    bool key = false;
    int64_t dts = 0;           /* ms，作为 LOC timestamp（解码时间） */
    /* pts - dts，即 FLV 的 CompositionTime。
     * 非负：FLV 封装时会整体抬高 PTS 保证它不出现负值。
     * 有 B 帧时非零，服务端要靠它才能还原正确的显示顺序。 */
    int64_t cts = 0;

    /* 视频：AVCC 负载（已剥掉 5B FLV video tag header）
     * 音频：裸 AAC（已剥掉 2B FLV audio tag header）
     * seq 帧：视频是 avcC 记录，音频是 ASC */
    std::vector<uint8_t> payload;
};

class FlvReader {
public:
    FlvReader();
    ~FlvReader();

    FlvReader(const FlvReader &) = delete;
    FlvReader &operator=(const FlvReader &) = delete;

    /* 打开 FLV 文件。不读数据，只把 fd/demuxer 准备好。
     * 真正的解析在 Fill() 里按需进行（连接建立后才开始）。 */
    int Open(const std::string &path);

    /* 队列空了就想办法再填一批。返回 false 表示文件已读完。 */
    bool Fill();

    bool Empty() const { return queue_.empty(); }
    const FlvFrame &Front() const { return queue_.front(); }
    /* 供调用方 move 走 payload，避免大帧拷贝 */
    FlvFrame TakeFront() {
        FlvFrame f = std::move(queue_.front());
        queue_.pop_front();
        return f;
    }

    size_t video_count() const { return video_count_; }
    size_t audio_count() const { return audio_count_; }
    size_t dropped_codec() const { return dropped_codec_; }
    size_t neg_cts() const { return neg_cts_; }
    int64_t duration_ms() const { return last_dts_; }
    bool Eof() const { return eof_; }

private:
    struct Sink;

    std::string path_;
    FILE *fp_ = nullptr;

    /* 声明顺序即析构逆序：sink_ 后声明 → 先析构，
     * 确保 demux_ 析构时 sink_ 仍然有效。 */
    std::unique_ptr<cpp_streamer::FlvDemuxer> demux_;
    std::unique_ptr<Sink> sink_;

    std::deque<FlvFrame> queue_;
    bool eof_ = false;

    size_t video_count_ = 0;
    size_t audio_count_ = 0;
    size_t dropped_codec_ = 0;
    size_t neg_cts_ = 0;
    int64_t last_dts_ = 0;
    bool warned_codec_ = false;
};

} /* namespace flv2moq */

#endif /* FLV2MOQ_FLV_READER_HPP */
