#ifndef RTMP_WRITER_HPP
#define RTMP_WRITER_HPP
#ifdef _WIN64
#define WIN32_LEAN_AND_MEAN  // ÆÁ±Î Windows ¾É°æÈßÓàÍ·ÎÄ¼þ£¨°üÀ¨ winsock.h£©
#endif
#include "rtmp_pub.hpp"
#include "rtmp_session.hpp"
#include "media_packet.hpp"
#include "utils/logger.hpp"
#include <memory>

namespace cpp_streamer
{
    class RtmpWriter : public AvWriterInterface
    {
    public:
        RtmpWriter(RtmpSession* session, Logger* logger);
        virtual ~RtmpWriter();

    public:
        virtual int WritePacket(Media_Packet_Ptr) override;
        virtual std::string GetKey() override;
        virtual std::string GetWriterId() override;
        virtual void CloseWriter() override;
        virtual bool IsInited() override;
        virtual void SetInitFlag(bool flag) override;

    private:
        RtmpSession* session_ = nullptr;
        Logger* logger_ = nullptr;
        bool init_flag_ = false;
        bool closed_ = false;
        std::string key_; // app/streamname
    };

    typedef std::shared_ptr<RtmpWriter> RTMP_WRITER_PTR;
}
#endif