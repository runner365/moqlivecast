#include "moq_source_c_api.h"

#include "media_source_sink.hpp"

#include "data_buffer.hpp"
#include "h264_h265_header.hpp"
#include "logger.h"
#include "media_packet.hpp"

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <vector>

#include <uv.h>

/* This file is a C ABI implementation but its internals use the server's
 * C++ media types (Media_Packet, MEDIA_CODEC_*, DataBuffer, AnnexB2Avcc). */
using namespace cpp_streamer;

namespace {

uv_loop_t* g_loop = nullptr;

std::mutex g_err_mutex;
char g_last_error[256] = {0};

void SetError(const char* what, int code) {
    std::lock_guard<std::mutex> lock(g_err_mutex);
    if (code != 0) {
        snprintf(g_last_error, sizeof(g_last_error), "%s: %s", what, strerror(-code));
    } else {
        snprintf(g_last_error, sizeof(g_last_error), "%s", what);
    }
}

/* Annex-B -> AVCC（长度前缀），与 flv_file_source / media_source_sink 的处理保持一致 */
bool ToAvcc(const uint8_t* data, size_t len, std::vector<uint8_t>* out) {
    if (!data || len == 0 || !out) return false;
    std::vector<std::shared_ptr<DataBuffer>> nalus;
    if (!AnnexB2Avcc(const_cast<uint8_t*>(data), len, nalus)) return false;
    out->clear();
    for (const auto& nalu : nalus) {
        if (!nalu || nalu->DataLen() == 0) continue;
        out->insert(out->end(),
                    reinterpret_cast<const uint8_t*>(nalu->Data()),
                    reinterpret_cast<const uint8_t*>(nalu->Data()) + nalu->DataLen());
    }
    return !out->empty();
}

int AdtsHeaderLen(const uint8_t* d, size_t n) {
    if (n < 7) return -1;
    if (d[0] != 0xFF || (d[1] & 0xF0) != 0xF0) return -1;
    return (d[1] & 0x01) ? 7 : 9;   /* protection_absent ? 7 : 9 */
}

/* ADTS 头 -> AudioSpecificConfig（2 字节），用于首次发布 catalog */
void AdtsToAsc(const uint8_t* d, uint8_t asc[2]) {
    const int profile = (d[2] >> 6) & 0x03;
    const int sfi     = (d[2] >> 2) & 0x0F;
    const int chan    = ((d[2] & 0x01) << 2) | ((d[3] >> 6) & 0x03);
    asc[0] = (uint8_t)((profile << 3) | (sfi >> 1));
    asc[1] = (uint8_t)(((sfi & 0x01) << 7) | (chan << 3));
}

} /* namespace */

struct moq_source {
    std::unique_ptr<cpp_streamer::MediaSourceSink> sink;
    std::string app;
    std::string stream;
    std::string id;
    std::atomic<bool> running{false};
    std::atomic<uint64_t> frames_in{0};
    std::atomic<uint64_t> frames_dropped{0};
    std::atomic<int64_t> last_dts{-1};
    std::atomic<int> video_codec{MEDIA_CODEC_H264};   /* 由 push_video_config 设置 */
    std::atomic<bool> audio_cfg_sent{false};
    std::vector<uint8_t> scratch;
};

extern "C" {

int moq_source_set_loop(void* uv_loop) {
    g_loop = reinterpret_cast<uv_loop_t*>(uv_loop);
    return g_loop ? 0 : -EINVAL;
}

moq_source_t* moq_source_create(const char* app, const char* stream,
                                const char* source_id) {
    if (!g_loop) {
        SetError("uv loop not set (call moq_source_set_loop first)", -EINVAL);
        return nullptr;
    }
    if (!app || !stream || !*app || !*stream) {
        SetError("app/stream required", -EINVAL);
        return nullptr;
    }
    moq_source_t* s = new (std::nothrow) moq_source();
    if (!s) {
        SetError("out of memory", -ENOMEM);
        return nullptr;
    }
    s->app = app;
    s->stream = stream;
    s->id = source_id ? source_id : "capi";
    s->sink.reset(new cpp_streamer::MediaSourceSink(g_loop, s->app, s->stream, s->id));
    LOG_INFO("[moq-source-capi] created app=%s stream=%s id=%s",
             s->app.c_str(), s->stream.c_str(), s->id.c_str());
    return s;
}

void moq_source_destroy(moq_source_t* s) {
    if (!s) return;
    if (s->running.load()) moq_source_stop(s);
    delete s;
}

int moq_source_start(moq_source_t* s) {
    if (!s) return -EINVAL;
    if (s->running.exchange(true)) return 0;   /* 幂等 */
    if (s->sink->Start() < 0) {
        s->running = false;
        SetError("sink start failed", -EIO);
        return -EIO;
    }
    LOG_INFO("[moq-source-capi] started app=%s stream=%s id=%s",
             s->app.c_str(), s->stream.c_str(), s->id.c_str());
    return 0;
}

void moq_source_stop(moq_source_t* s) {
    if (!s) return;
    if (!s->running.exchange(false)) return;
    s->sink->Stop();
    LOG_INFO("[moq-source-capi] stopped app=%s stream=%s frames_in=%llu dropped=%llu",
             s->app.c_str(), s->stream.c_str(),
             static_cast<unsigned long long>(s->frames_in.load()),
             static_cast<unsigned long long>(s->frames_dropped.load()));
}

int moq_source_push_video_config(moq_source_t* s, moq_codec_t codec,
                                 const uint8_t* extradata, size_t len) {
    if (!s || !extradata || len == 0) return -EINVAL;
    cpp_streamer::TrackInfo info;
    info.id = cpp_streamer::MediaTrackId::Video;
    info.codec = (codec == MOQ_CODEC_H265) ? MEDIA_CODEC_H265 : MEDIA_CODEC_H264;
    s->video_codec.store(static_cast<int>(info.codec));
    info.extradata.assign(extradata, extradata + len);
    const int ret = s->sink->onTrackInfo(info);
    LOG_INFO("[moq-source-capi] video config codec=%d extradata=%zuB ret=%d",
             static_cast<int>(info.codec), info.extradata.size(), ret);
    if (ret < 0) SetError("onTrackInfo(video) rejected", ret);
    return ret;
}

int moq_source_push_audio_config(moq_source_t* s,
                                 const uint8_t* extradata, size_t len) {
    if (!s || !extradata || len == 0) return -EINVAL;
    cpp_streamer::TrackInfo info;
    info.id = cpp_streamer::MediaTrackId::Audio;
    info.codec = MEDIA_CODEC_AAC;
    info.extradata.assign(extradata, extradata + len);
    const int ret = s->sink->onTrackInfo(info);
    LOG_INFO("[moq-source-capi] audio config extradata=%zuB ret=%d",
             info.extradata.size(), ret);
    if (ret < 0) SetError("onTrackInfo(audio) rejected", ret);
    return ret;
}

int moq_source_push_video(moq_source_t* s, const moq_video_frame_t* f) {
    if (!s || !f || !f->data || f->len == 0) return -EINVAL;
    if (!s->running.load()) {
        SetError("source not started", -EINVAL);
        return -EINVAL;
    }

    const uint8_t* payload = f->data;
    size_t len = f->len;
    if (f->format == MOQ_BITSTREAM_ANNEXB) {
        if (!ToAvcc(f->data, f->len, &s->scratch)) {
            SetError("annexb to avcc failed", -EINVAL);
            return -EINVAL;
        }
        payload = s->scratch.data();
        len = s->scratch.size();
    }

    auto pkt = std::make_shared<Media_Packet>(len + 32);
    pkt->av_type_ = MEDIA_VIDEO_TYPE;
    pkt->codec_type_ = static_cast<MEDIA_CODEC_TYPE>(s->video_codec.load());
    pkt->fmt_type_ = MEDIA_FORMAT_FLV;
    pkt->nalu_fmt_type_ = NALU_FORMAT_AVCC;
    pkt->dts_ = f->dts_ms;
    pkt->pts_ = f->pts_ms;
    pkt->is_key_frame_ = f->is_keyframe ? true : false;
    pkt->is_seq_hdr_ = false;
    pkt->app_ = s->app;
    pkt->streamname_ = s->stream;
    pkt->key_ = s->app + "/" + s->stream;
    pkt->buffer_ptr_->AppendData(reinterpret_cast<const char*>(payload), len);

    const int ret = s->sink->onVideo(pkt);
    const uint64_t n = s->frames_in.load();
    if ((n == 0) || ((n % 300) == 0)) {
        LOG_INFO("[moq-source-capi] push video dts=%lld key=%d len=%zu ret=%d",
                 static_cast<long long>(f->dts_ms), f->is_keyframe, len, ret);
    }
    if (ret == 0) {
        s->frames_in.fetch_add(1);
        s->last_dts.store(f->dts_ms);
    } else {
        s->frames_dropped.fetch_add(1);
        if (ret != -EAGAIN) SetError("onVideo rejected", ret);
    }
    return ret;
}

int moq_source_push_audio(moq_source_t* s, const uint8_t* data, size_t len,
                          int64_t dts_ms, int64_t pts_ms,
                          moq_audio_format_t format) {
    if (!s || !data || len == 0) return -EINVAL;
    if (!s->running.load()) {
        SetError("source not started", -EINVAL);
        return -EINVAL;
    }

    const uint8_t* payload = data;
    size_t plen = len;

    if (format == MOQ_AUDIO_AAC_ADTS) {
        const int hdr = AdtsHeaderLen(data, len);
        if (hdr < 0 || (size_t)hdr >= len) {
            SetError("invalid ADTS header", -EBADMSG);
            return -EBADMSG;
        }
        /* 第一次见到 ADTS 时，自动推导 AudioSpecificConfig 并发布 catalog */
        if (!s->audio_cfg_sent.load()) {
            uint8_t asc[2] = {0};
            AdtsToAsc(data, asc);
            cpp_streamer::TrackInfo info;
            info.id = cpp_streamer::MediaTrackId::Audio;
            info.codec = MEDIA_CODEC_AAC;
            info.extradata.assign(asc, asc + sizeof(asc));
            s->sink->onTrackInfo(info);
            s->audio_cfg_sent.store(true);
        }
        payload = data + hdr;
        plen = len - hdr;
    }

    auto pkt = std::make_shared<Media_Packet>(plen + 32);
    pkt->av_type_ = MEDIA_AUDIO_TYPE;
    pkt->codec_type_ = MEDIA_CODEC_AAC;
    pkt->fmt_type_ = MEDIA_FORMAT_FLV;
    pkt->dts_ = dts_ms;
    pkt->pts_ = pts_ms;
    pkt->is_key_frame_ = true;
    pkt->is_seq_hdr_ = false;
    pkt->app_ = s->app;
    pkt->streamname_ = s->stream;
    pkt->key_ = s->app + "/" + s->stream;
    pkt->buffer_ptr_->AppendData(reinterpret_cast<const char*>(payload), plen);

    const int ret = s->sink->onAudio(pkt);
    const uint64_t n = s->frames_in.load();
    if ((n == 0) || ((n % 300) == 0)) {
        LOG_INFO("[moq-source-capi] push audio dts=%lld len=%zu ret=%d",
                 static_cast<long long>(dts_ms), plen, ret);
    }
    if (ret == 0) {
        s->frames_in.fetch_add(1);
    } else {
        s->frames_dropped.fetch_add(1);
        if (ret != -EAGAIN) SetError("onAudio rejected", ret);
    }
    return ret;
}

void moq_source_get_stats(moq_source_t* s, moq_source_stats_t* out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (!s) return;
    out->frames_in = s->frames_in.load();
    out->frames_dropped = s->frames_dropped.load();
    out->last_dts_ms = s->last_dts.load();
    out->running = s->running.load() ? 1 : 0;
}

const char* moq_source_last_error(void) {
    static thread_local char copy[256];
    std::lock_guard<std::mutex> lock(g_err_mutex);
    memcpy(copy, g_last_error, sizeof(copy));
    copy[sizeof(copy) - 1] = '\0';
    return copy;
}

} /* extern "C" */
