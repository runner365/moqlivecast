#ifndef GOP_CACHE_HPP
#define GOP_CACHE_HPP

#include "media_packet.hpp"
#include "logger.hpp"

#include <list>

namespace cpp_streamer {

/* 某一路 app/stream 的 GOP：视频关键帧到来时清空列表再放入。
 * 音/视频 sequence header 单独保存。 */
class GopCache {
public:
    explicit GopCache(Logger *logger = nullptr, uint32_t min_gop = 1);
    ~GopCache() = default;

    void Input(Media_Packet_Ptr pkt);
    size_t InsertPacket(Media_Packet_Ptr pkt);
    int WriterGop(AvWriterInterface *writer);
    void Clear();

    const std::list<Media_Packet_Ptr> &Packets() const { return packets_; }
    Media_Packet_Ptr AudioHdr() const { return audio_hdr_; }
    Media_Packet_Ptr VideoHdr() const { return video_hdr_; }

private:
    std::list<Media_Packet_Ptr> packets_;
    Media_Packet_Ptr audio_hdr_;
    Media_Packet_Ptr video_hdr_;
    Logger *logger_ = nullptr;
    uint32_t min_gop_ = 1;
};

} /* namespace cpp_streamer */

#endif /* GOP_CACHE_HPP */
