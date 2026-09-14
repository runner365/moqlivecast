#ifndef FLV_DEMUXER_HPP
#define FLV_DEMUXER_HPP
#include "data_buffer.hpp"
#include "cpp_streamer_interface.hpp"
#include "logger.hpp"
#include "wait_basedon_timestamp.hpp"

#include <map>

extern "C" {
void* make_flvdemux_streamer();
void destroy_flvdemux_streamer(void* streamer);
}

namespace cpp_streamer
{
#define FLV_RET_NEED_MORE    1

#define FLV_HEADER_LEN     9
#define FLV_TAG_PRE_SIZE   4
#define FLV_TAG_HEADER_LEN 11

class FlvDemuxer : CppStreamerInterface
{
public:
    FlvDemuxer(bool support_flv_media_hdr, Logger* logger = nullptr);
    virtual ~FlvDemuxer();

public:
    virtual std::string StreamerName() override;
    virtual void SetLogger(Logger* logger) override {
        logger_ = logger;
    }
    virtual int AddSinker(CppStreamerInterface* sinker) override;
    virtual int RemoveSinker(const std::string& name) override;
    virtual int SourceData(Media_Packet_Ptr pkt_ptr) override;
    virtual void StartNetwork(const std::string& url, void* loop_handle) override {}
    virtual void AddOption(const std::string& key, const std::string& value) override;
    virtual void SetReporter(StreamerReport* reporter) override;

public:
    void SetSupportFlvMediaHeader(bool enable) {
        support_flv_media_hdr_ = enable;
    }
    bool GetSupportFlvMediaHeader() {
        return support_flv_media_hdr_;
    }
private:
    int InputPacket(Media_Packet_Ptr pkt_ptr);
    int InputPacket(const uint8_t* data, size_t data_len, const std::string& key);
    bool HasVideo() {return has_video_;}
    bool HasAudio() {return has_audio_;}
    int HandlePacket();
    int SinkData(Media_Packet_Ptr pkt_ptr);
    int DecodeMetaData(uint8_t* data, int len, Media_Packet_Ptr pkt_ptr);
    void Report(const std::string& type, const std::string& value);

private:
    int HandleAudioTag(uint8_t* data, int data_len, Media_Packet_Ptr pkt_ptr);
    int HandleVideoTag(uint8_t* data, int data_len, Media_Packet_Ptr pkt_ptr);
    int HandleVideoSeqHdr(uint8_t* data, int data_len, Media_Packet_Ptr pkt_ptr);

private:
    static std::map<std::string, std::string> def_options_;

private:
    DataBuffer buffer_;
    std::string key_;
    bool has_video_ = false;
    bool has_audio_ = false;

private:
    bool flv_header_ready_ = false;
    bool tag_header_ready_ = false;

private:
    uint8_t tag_type_ = 0;
    uint32_t tag_data_size_ = 0;
    uint32_t tag_timestamp_ = 0;

private:
    int aac_asc_type_ = ASC_TYPE_AAC_LC;

private:
    WaitBasedOnTimestamp waiter_;

private:
    /* true：输出 FLV tag 媒体头(0x17/0xaf…)；false：剥头后输出 raw。不输出 FLV 文件头 */
    bool support_flv_media_hdr_ = false;
};

}

#endif //FLV_DEMUXER_HPP
