#ifndef HTTP_FLV_WRITER_HPP
#define HTTP_FLV_WRITER_HPP
#ifdef _WIN64
#define WIN32_LEAN_AND_MEAN  // ÆÁ±Î Windows ¾É°æÈßÓàÍ·ÎÄ¼þ£¨°üÀ¨ winsock.h£©
#endif
#include "net/http/http_common.hpp"
#include "media_packet.hpp"
#include "utils/logger.hpp"

namespace cpp_streamer
{

    class HttpFlvWriter : public AvWriterInterface
    {
    public:
        HttpFlvWriter(std::string key, std::string id, std::shared_ptr<HttpResponse> resp, Logger* logger, bool has_video = true, bool has_audio = true);
        virtual ~HttpFlvWriter();

    public:
        virtual int WritePacket(Media_Packet_Ptr) override;
        virtual std::string GetKey() override;
        virtual std::string GetWriterId() override;
        virtual void CloseWriter() override;
        virtual bool IsInited() override;
        virtual void SetInitFlag(bool flag) override;

    public:
        bool IsAlive();

    private:
        int SendFlvHeader();

    private:
        std::shared_ptr<HttpResponse> resp_;
        std::string key_;
        std::string writer_id_;
        bool init_flag_ = false;
        bool has_video_ = true;
        bool has_audio_ = true;
        bool flv_header_ready_ = false;
        bool closed_flag_ = false;
        int64_t alive_ms_ = -1;

    private:
		Logger* logger_ = nullptr;
    };
}


#endif