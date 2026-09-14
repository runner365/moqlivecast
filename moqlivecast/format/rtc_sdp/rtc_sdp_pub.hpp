#ifndef RTC_SDP_PUB_HPP
#define RTC_SDP_PUB_HPP
#include <string>
#include <stdint.h>
#include <stddef.h>
#include <vector>
#include <map>

namespace cpp_streamer
{

typedef enum RtcSdpType {
    RTC_SDP_UNKNOWN = 0,
    RTC_SDP_OFFER,
    RTC_SDP_ANSWER
} RtcSdpType;

typedef enum RtcSetupType {
    RTC_SETUP_UNKNOWN = 0,
    RTC_SETUP_ACTIVE,
    RTC_SETUP_PASSIVE,
    RTC_SETUP_ACTPASS
} RtcSetupType;

typedef enum RtcNetType {
    RTC_NET_UNKNOWN = 0,
    RTC_NET_TCP,
    RTC_NET_UDP
} RtcNetType;

typedef enum DirectionType {
    DIRECTION_UNKNOWN = 0,
    DIRECTION_SENDONLY,
    DIRECTION_RECVONLY,
    DIRECTION_SENDRECV
} DirectionType;

class H264CodecFmtpParam
{
public:
    H264CodecFmtpParam() = default;
    ~H264CodecFmtpParam() = default;
public:
    std::string Dump() {
        std::string fmtp;
        fmtp += "level-asymmetry-allowed=" + std::to_string(level_asymmetry_allowed_);
        fmtp += ";packetization-mode=" + std::to_string(packetization_mode_);
        fmtp += ";profile-level-id=" + profile_level_id_;
        return fmtp;
    }
public:
    int level_asymmetry_allowed_ = 0;
    int packetization_mode_ = 0;
    std::string profile_level_id_;
};

class AV1CodecFmtpParam
{
public:
    AV1CodecFmtpParam() = default;
    ~AV1CodecFmtpParam() = default;

public:
    std::string Dump() {
        std::string fmtp;
        fmtp += "level=" + std::to_string(level_idx_);
        fmtp += ";profile=" + std::to_string(profile_);
        fmtp += ";tier=" + std::to_string(tier_);
        return fmtp;
    }
public:
    int level_idx_ = -1;
    int profile_ = -1;
    int tier_ = -1;
};

class VP9CodecFmtpParam
{
public:
    VP9CodecFmtpParam() = default;
    ~VP9CodecFmtpParam() = default;

public:
    std::string Dump() {
        std::string fmtp;
        fmtp += "profile-id=" + std::to_string(profile_id_);
        return fmtp;
    }
public:
    int profile_id_ = -1;
};

class OpusCodecFmtpParam
{
public:
    OpusCodecFmtpParam() = default;
    ~OpusCodecFmtpParam() = default;

public:
    std::string Dump() {
        std::string fmtp;
        fmtp += "minptime=" + std::to_string(minptime_);
        fmtp += ";useinbandfec=" + std::to_string(useinbandfec_);
        return fmtp;
    }
public:
    int minptime_ = 0;
    int useinbandfec_ = 0;
    int maxaveragebitrate_ = 0;
    int stereo_ = 0;
    int sprop_stereo_ = 0;
};

class SsrcInfo
{
public:
    SsrcInfo() = default;
    ~SsrcInfo() = default;

public:
    uint32_t ssrc_ = 0;
    bool is_main_ = true;
    std::string cname_;
    std::string stream_id_;//stream id: msid's second part
    std::string track_id_;//track id: msid's third part
};

class ExtensionInfo
{
public:
    ExtensionInfo() = default;
    ~ExtensionInfo() = default;

public:
    int id_ = -1;
    std::string uri_;
};

}

#endif // RTC_SDP_PUB_HPP