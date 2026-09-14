#ifndef RTMP_MEDIA_STREAM_HPP
#define RTMP_MEDIA_STREAM_HPP
#ifdef _WIN64
#define WIN32_LEAN_AND_MEAN  // ùùùù Windows ù?ùùùùù?ù?ùùùùùùù winsock.hùù
#endif
#include "media_packet.hpp"
#include "gop_cache.hpp"
#include "logger.hpp"
#include <stdint.h>
#include <unordered_map>
#include <string>
#include <memory>
#include <vector>
#include <list>

namespace cpp_streamer
{

    typedef std::unordered_map<std::string, AvWriterInterface*> WRITER_MAP;

    typedef void (*PLAY_CALLBACK)(const std::string& key);

    class MediaStream
    {
    public:
        MediaStream(Logger* logger = nullptr);
        ~MediaStream();

    public:
		Logger* logger_ = nullptr;
        std::string stream_key_;//app/streamname
        bool publisher_exist_ = false;
        bool clock_reset_pending_ = false;
        int64_t last_in_dts_ = -1;
        int64_t last_out_dts_ = -1;
        int64_t ts_shift_ = 0;
        GopCache cache_;
        WRITER_MAP writer_map_;//(session_key, av_writer_base*)

        void NormalizeDts(Media_Packet_Ptr pkt);
    };

    typedef std::shared_ptr<MediaStream> MEDIA_STREAM_PTR;

    class StreamManagerCallbackI
    {
    public:
        virtual void OnPublish(const std::string& app, const std::string& streamname) = 0;
        virtual void OnUnpublish(const std::string& app, const std::string& streamname) = 0;
    };

    class MediaStreamManager
    {
    public:
        static int AddPlayer(AvWriterInterface* writer_p);
        static void RemovePlayer(AvWriterInterface* writer_p);

        static MEDIA_STREAM_PTR AddPublisher(const std::string& stream_key);
        static void RemovePublisher(const std::string& stream_key);

        static void SetHlsWriter(AvWriterInterface* writer);
        static void SetRtcWriter(AvWriterInterface* writer);

        static void SetPlayCallback(PLAY_CALLBACK cb);
        static PLAY_CALLBACK GetPlayCallback();

        static void SetLogger(Logger* logger);
        static Logger* GetLogger();

    public:
        static int WriterMediaPacket(Media_Packet_Ptr pkt_ptr);
        static GopCache *GetGop(const std::string &stream_key);
        static bool HasGopMedia(const std::string &stream_key);

    public:
        static void AddStreamCallback(StreamManagerCallbackI* cb) {
            cb_vec_.push_back(cb);
        }

    private:
        static bool GetAppStreamname(const std::string& stream_key, std::string& app, std::string& streamname);

    private:
        static std::unordered_map<std::string, MEDIA_STREAM_PTR> media_streams_map_;//key("app/stream"), MEDIA_STREAM_PTR
        static std::vector<StreamManagerCallbackI*> cb_vec_;

    private:
        static AvWriterInterface* hls_writer_;
        static AvWriterInterface* r2r_writer_;//rtmp to webrtc

    private:
        static PLAY_CALLBACK play_cb_;
		static Logger* logger_;
    };
}
#endif //RTMP_MEDIA_STREAM_HPP