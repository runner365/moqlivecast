#ifndef FLV_PUB_HPP
#define FLV_PUB_HPP
#ifdef _WIN64
#define WIN32_LEAN_AND_MEAN  // 屏蔽 Windows 旧版冗余头文件（包括 winsock.h）
#endif
#include "media_packet.hpp"
#include "logger.hpp"

namespace cpp_streamer
{
#define FLV_TAG_AUDIO     0x08
#define FLV_TAG_VIDEO     0x09

#define FLV_TAG_META_DATA3 15
#define FLV_TAG_AMF3       17
#define FLV_TAG_META_DATA0 18
#define FLV_TAG_AMF0       20

#define FLV_VIDEO_KEY_FLAG   0x10
#define FLV_VIDEO_INTER_FLAG 0x20

#define FLV_VIDEO_AVC_SEQHDR 0x00
#define FLV_VIDEO_AVC_NALU   0x01
#define FLV_VIDEO_AVC_END    0x02

#define FLV_VIDEO_H264_CODEC 0x07
#define FLV_VIDEO_H265_CODEC 0x0c

#define FLV_AUDIO_MP3_CODEC  0x20
#define FLV_AUDIO_EX_HEADER  0x90
#define FLV_AUDIO_AAC_CODEC  0xa0

/* offsets for packed values */
#define FLV_AUDIO_SAMPLESSIZE_OFFSET 1
#define FLV_AUDIO_SAMPLERATE_OFFSET  2
#define FLV_AUDIO_CODECID_OFFSET     4

enum {
    FLV_MONO   = 0,
    FLV_STEREO = 1,
};

enum {
    FLV_SAMPLESSIZE_8BIT  = 0,
    FLV_SAMPLESSIZE_16BIT = 1 << FLV_AUDIO_SAMPLESSIZE_OFFSET,
};

enum {
    FLV_SAMPLERATE_SPECIAL = 0, /**< signifies 5512Hz and 8000Hz in the case of NELLYMOSER */
    FLV_SAMPLERATE_11025HZ = 1 << FLV_AUDIO_SAMPLERATE_OFFSET,
    FLV_SAMPLERATE_22050HZ = 2 << FLV_AUDIO_SAMPLERATE_OFFSET,
    FLV_SAMPLERATE_44100HZ = 3 << FLV_AUDIO_SAMPLERATE_OFFSET,
};

typedef enum {
    AUDIO_PKTTYPE_SEQUENCE_START = 0,
    AUDIO_PKTTYPE_CODED_FRAMES = 1,
    AUDIO_PKTTYPE_SEQUENCE_END = 2,
    AUDIO_PKTTYPE_RESERVED1 = 3,
    AUDIO_PKTTYPE_MULTI_CHANNEL_CFG = 4,
    AUDIO_PKTTYPE_MULTI_TRACK = 5,
    AUDIO_PKTTYPE_RESERVED2 = 6,
    AUDIO_PKTTYPE_MODEX = 7,
} FLV_AUDIO_PACKET_TYPE;

typedef enum {
    VIDEO_PKTTYPE_SEQUENCE_START = 0,
    VIDEO_PKTTYPE_CODEDFRAMES = 1,
    VIDEO_PKTTYPE_SEQUENCE_END = 2,
    VIDEO_PKTTYPE_CODED_FRAMES_X = 3,
    VIDEO_PKTTYPE_META_DATA = 4,
    VIDEO_PKTTYPE_MPEG2TS_SEQUENCE_START = 5,
    VIDEO_PKTTYPE_MULTITRACK = 6,
    VIDEO_PKTTYPE_MODEX = 7,
} FLV_VIDEO_PACKET_TYPE;

typedef enum {
    OneTrack = 0,
    ManyTracks = 1,
    ManyTracksManyCodecs = 2,
} AvMultitrackType;

typedef enum {
    VIDEO_FRAME_RESERVED = 0,
    VIDEO_KEY_FRAME = 1,
    VIDEO_INTER_FRAME = 2,
    VIDEO_DISPOSABLE_INTER_FRAME = 3,
    VIDEO_GENERATED_KEY_FRAME = 4,
    VIDEO_COMMAND_FRAME = 5,
} FlvVideoFrameType;

/*
ASC flag：xxxx xyyy yzzz z000
x： aac type，类型2表示AAC-LC，5是SBR, 29是ps，5和29比较特殊ascflag的长度会变成4；
y:  sample rate, 采样率, 7表示22050采样率
z:  通道数，2是双通道
*/

int AddFlvMediaHeader(Media_Packet_Ptr pkt_ptr, Logger* logger);

Media_Packet_Ptr GetFlvMediaPacket(uint8_t type_id, uint32_t ts, const uint8_t* data, int len, Logger* logger);

MEDIA_CODEC_TYPE GetVideoCodecIdByFlvCodec(uint32_t flv_codec);
MEDIA_CODEC_TYPE GetAudioCodecIdByFlvCodec(uint32_t flv_codec);
}

#endif

