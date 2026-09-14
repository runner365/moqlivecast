#include "moq_handler.hpp"
#include "moq/moq_push.hpp"
#include "flv_demux.hpp"
#include "flv_pub.hpp"
#include "media_packet.hpp"
#include "media_stream_manager.hpp"
#include "cpp_streamer_interface.hpp"
#include "gop_cache.hpp"
#include "utils/meta_log.hpp"
#include "logger.h"

#include <cstring>
#include <vector>

namespace cpp_streamer {

namespace {

std::string MediaKey(const std::string &app, const std::string &stream) {
    return app + "/" + stream;
}

class FlvPushIngest : public CppStreamerInterface {
public:
    explicit FlvPushIngest(std::string key) : key_(std::move(key)) {
        name_ = "flv-ingest:" + key_;
    }

    std::string StreamerName() override { return name_; }
    void SetLogger(Logger *logger) override { logger_ = logger; }
    int AddSinker(CppStreamerInterface * /*sinker*/) override { return 0; }
    int RemoveSinker(const std::string & /*name*/) override { return 0; }
    int SourceData(Media_Packet_Ptr pkt) override {
        if (!pkt) return 0;
        if (pkt->key_.empty()) pkt->key_ = key_;
        if (pkt->app_.empty() || pkt->streamname_.empty()) {
            auto pos = key_.find('/');
            if (pos != std::string::npos) {
                pkt->app_ = key_.substr(0, pos);
                pkt->streamname_ = key_.substr(pos + 1);
            }
        }
        return MediaStreamManager::WriterMediaPacket(pkt);
    }
    void StartNetwork(const std::string & /*url*/, void * /*loop*/) override {}
    void AddOption(const std::string & /*key*/, const std::string & /*value*/) override {}
    void SetReporter(StreamerReport *reporter) override { report_ = reporter; }

private:
    std::string key_;
};

std::unordered_map<std::string, std::unique_ptr<FlvDemuxer>> g_flv_demux;
std::unordered_map<std::string, std::unique_ptr<FlvPushIngest>> g_flv_ingest;

} /* namespace */

class WtFlvWriter : public AvWriterInterface {
public:
    WtFlvWriter(WTServerSession &sess, WTServerStream &st)
        : sess_(&sess)
        , st_(&st)
        , key_(sess.MediaPath())
        , id_(sess.SessionId()) {
    }

    int WritePacket(Media_Packet_Ptr pkt) override {
        if (closed_ || !st_ || !st_->Valid() || !pkt) return -1;
        if (!header_sent_) {
            WriteFlvFileHeader();
            header_sent_ = true;
        }

        const bool replaying = replay_left_ > 0;
        if (replaying) replay_left_--;

        if (wait_key_) {
            if (pkt->is_seq_hdr_) {
                WriteFlvTag(pkt, PullTs(pkt));
                return 0;
            }
            if (pkt->av_type_ == MEDIA_VIDEO_TYPE && pkt->is_key_frame_) {
                wait_key_ = false;
                LOG_INFO("[wt-flv] pull start from key dts=%lld", (long long)pkt->dts_);
                WriteFlvTag(pkt, PullTs(pkt));
                return 0;
            }
            return 0;
        }

        if (replaying && last_video_dts_ >= 0 &&
            pkt->av_type_ == MEDIA_AUDIO_TYPE &&
            pkt->dts_ > last_video_dts_ + 40) {
            return 0;
        }

        WriteFlvTag(pkt, PullTs(pkt));
        return 0;
    }

    std::string GetKey() override { return key_; }
    std::string GetWriterId() override { return id_; }

    void CloseWriter() override {
        closed_ = true;
    }

    bool IsInited() override { return init_flag_; }

    void SetInitFlag(bool flag) override {
        init_flag_ = flag;
        if (!flag) return;
        GopCache *gop = MediaStreamManager::GetGop(key_);
        if (!gop) return;

        replay_left_ = 0;
        if (gop->VideoHdr()) replay_left_++;
        if (gop->AudioHdr()) replay_left_++;
        replay_left_ += gop->Packets().size();

        int64_t last_v = -1;
        int64_t last_a = -1;
        for (const auto &p : gop->Packets()) {
            if (!p) continue;
            if (p->av_type_ == MEDIA_VIDEO_TYPE) last_v = p->dts_;
            if (p->av_type_ == MEDIA_AUDIO_TYPE) last_a = p->dts_;
        }
        last_video_dts_ = last_v;
        if (last_v >= 0 && last_a >= 0 && last_a - last_v > 400) {
            wait_key_ = true;
            LOG_INFO("[wt-flv] pull skip stale gop video_end=%lld audio_end=%lld",
                     (long long)last_v, (long long)last_a);
            return;
        }

        int64_t origin = -1;
        for (const auto &p : gop->Packets()) {
            if (!p || p->is_seq_hdr_ || p->dts_ <= 0) continue;
            if (last_v >= 0 && p->av_type_ == MEDIA_AUDIO_TYPE &&
                p->dts_ > last_v + 40) {
                continue;
            }
            if (origin < 0 || p->dts_ < origin) origin = p->dts_;
        }
        if (origin >= 0 && ts_base_ < 0) {
            ts_base_ = origin;
            LOG_INFO("[wt-flv] pull ts_base=%lld (gop min dts)", (long long)ts_base_);
        }
    }

private:
    void WriteFlvFileHeader() {
        static const uint8_t hdr[13] = {
            0x46, 0x4c, 0x56, 0x01, 0x05,
            0x00, 0x00, 0x00, 0x09,
            0x00, 0x00, 0x00, 0x00,
        };
        st_->Write(hdr, sizeof(hdr));
        LOG_INFO("[wt-flv] pull send flv header key=%s flags=av", key_.c_str());
    }

    uint32_t PullTs(Media_Packet_Ptr pkt) {
        if (!pkt) return 0;
        const int64_t dts = pkt->dts_ < 0 ? 0 : pkt->dts_;
        if (pkt->is_seq_hdr_ || dts <= 0) {
            if (ts_base_ < 0) return 0;
            const int64_t t = dts - ts_base_;
            return t < 0 ? 0 : static_cast<uint32_t>(t);
        }
        if (ts_base_ < 0) {
            ts_base_ = dts;
            LOG_INFO("[wt-flv] pull ts_base=%lld", (long long)ts_base_);
        }
        const int64_t t = dts - ts_base_;
        return t < 0 ? 0 : static_cast<uint32_t>(t);
    }

    void WriteFlvTag(Media_Packet_Ptr pkt, uint32_t ts) {
        if (!st_ || !st_->Valid() || !pkt || !pkt->buffer_ptr_) return;
        const auto *body = reinterpret_cast<const uint8_t *>(pkt->buffer_ptr_->Data());
        const size_t body_len = pkt->buffer_ptr_->DataLen();
        if (!body || body_len == 0) return;

        uint8_t tag_type = FLV_TAG_META_DATA0;
        if (pkt->av_type_ == MEDIA_VIDEO_TYPE) tag_type = FLV_TAG_VIDEO;
        else if (pkt->av_type_ == MEDIA_AUDIO_TYPE) tag_type = FLV_TAG_AUDIO;
        const uint32_t prev = static_cast<uint32_t>(11 + body_len);

        std::vector<uint8_t> out(11 + body_len + 4);
        out[0] = tag_type;
        out[1] = static_cast<uint8_t>((body_len >> 16) & 0xff);
        out[2] = static_cast<uint8_t>((body_len >> 8) & 0xff);
        out[3] = static_cast<uint8_t>(body_len & 0xff);
        out[4] = static_cast<uint8_t>((ts >> 16) & 0xff);
        out[5] = static_cast<uint8_t>((ts >> 8) & 0xff);
        out[6] = static_cast<uint8_t>(ts & 0xff);
        out[7] = static_cast<uint8_t>((ts >> 24) & 0xff);
        out[8] = 0;
        out[9] = 0;
        out[10] = 0;
        memcpy(out.data() + 11, body, body_len);
        out[11 + body_len]     = static_cast<uint8_t>((prev >> 24) & 0xff);
        out[11 + body_len + 1] = static_cast<uint8_t>((prev >> 16) & 0xff);
        out[11 + body_len + 2] = static_cast<uint8_t>((prev >> 8) & 0xff);
        out[11 + body_len + 3] = static_cast<uint8_t>(prev & 0xff);
        st_->Write(out.data(), out.size());
    }

    WTServerSession *sess_ = nullptr;
    WTServerStream *st_ = nullptr;
    std::string key_;
    std::string id_;
    bool init_flag_ = false;
    bool closed_ = false;
    bool header_sent_ = false;
    bool wait_key_ = false;
    int64_t ts_base_ = -1;
    int64_t last_video_dts_ = -1;
    size_t replay_left_ = 0;
};

std::unordered_map<std::string, std::unique_ptr<WtFlvWriter>> MoqHandler::pull_writers_;

void MoqHandler::FeedFlv(const std::string &app, const std::string &stream,
                         const uint8_t *data, size_t len) {
    if (app.empty() || stream.empty() || !data || len == 0) return;
    const std::string key = MediaKey(app, stream);
    if (!g_flv_demux.count(key)) {
        auto ingest = std::make_unique<FlvPushIngest>(key);
        auto demux = std::make_unique<FlvDemuxer>(true, nullptr);
        demux->AddSinker(ingest.get());
        g_flv_ingest[key] = std::move(ingest);
        g_flv_demux[key] = std::move(demux);
        LOG_INFO("[wt-flv] add demux %s", key.c_str());
    }
    Media_Packet_Ptr pkt = std::make_shared<Media_Packet>(len);
    pkt->app_ = app;
    pkt->streamname_ = stream;
    pkt->key_ = key;
    pkt->buffer_ptr_->AppendData(reinterpret_cast<const char *>(data), len);
    g_flv_demux[key]->SourceData(pkt);
}

void MoqHandler::DropPublisher(const std::string &app, const std::string &stream) {
    const std::string key = MediaKey(app, stream);
    g_flv_demux.erase(key);
    g_flv_ingest.erase(key);
    MediaStreamManager::RemovePublisher(key);
    LOG_INFO("[wt-flv] drop publisher %s", key.c_str());
}

void MoqHandler::OnSession(WTServerSession &sess, const std::string &path) {
    if (MoqRfcHandler::IsMoqPath(path)) {
        MoqRfcHandler::OnSession(sess, path);
        return;
    }
    const char *ev = (sess.Method() == "pull") ? "wt_pull" : "wt_push";
    MetaLog::Instance().Event(ev, "opensession",
                              sess.App(), sess.Stream(), sess.ExtraParams());
    LOG_DEBUG("[moq] session path=%s method=%s app=%s stream=%s id=%s",
             path.c_str(), sess.Method().c_str(),
             sess.App().c_str(), sess.Stream().c_str(),
             sess.SessionId().c_str());
}

void MoqHandler::OnStreamData(WTServerSession &sess, WTServerStream &st,
                              const uint8_t *data, size_t len,
                              const std::string &path) {
    if (MoqRfcHandler::IsMoqPath(path)) {
        MoqRfcHandler::OnStreamData(sess, st, data, len, path);
        return;
    }
    LOG_DEBUG("[moq] path=%s method=%s app=%s stream=%s recv %zu bytes",
             path.c_str(), sess.Method().c_str(),
             sess.App().c_str(), sess.Stream().c_str(), len);
    if (sess.Method() == "pull") {
        OnPullOpen(sess, st);
        return;
    }
    if (sess.Method() == "push") {
        if (MetaLog::Instance().FirstStream(sess.SessionId())) {
            MetaLog::Instance().Event("wt_push", "openstream",
                                      sess.App(), sess.Stream(),
                                      sess.ExtraParams());
        }
        sess.StartMetaStats("wt_push_stats", "stream_push");
        FeedFlv(sess.App(), sess.Stream(), data, len);
    }
}

void MoqHandler::OnSessionClose(WTServerSession &sess, const std::string &path) {
    if (MoqRfcHandler::IsMoqPath(path)) {
        MoqRfcHandler::OnSessionClose(sess, path);
        return;
    }
    const char *ev = (sess.Method() == "pull") ? "wt_pull" : "wt_push";
    sess.StopMetaStats();
    MetaLog::Instance().ForgetSession(sess.SessionId());
    MetaLog::Instance().Event(ev, "closesession",
                              sess.App(), sess.Stream(), sess.ExtraParams());
    LOG_INFO("[moq] session closed path=%s method=%s app=%s stream=%s id=%s",
             path.c_str(), sess.Method().c_str(),
             sess.App().c_str(), sess.Stream().c_str(),
             sess.SessionId().c_str());
    if (sess.Method() == "pull") {
        auto it = pull_writers_.find(sess.SessionId());
        if (it != pull_writers_.end()) {
            MediaStreamManager::RemovePlayer(it->second.get());
            pull_writers_.erase(it);
        }
    } else if (sess.Method() == "push") {
        DropPublisher(sess.App(), sess.Stream());
    }
}

void MoqHandler::OnPullOpen(WTServerSession &sess, WTServerStream &st) {
    const std::string &id = sess.SessionId();
    if (pull_writers_.count(id)) return;
    LOG_INFO("[wt-flv] pull open stream app=%s stream=%s id=%s",
             sess.App().c_str(), sess.Stream().c_str(), id.c_str());
    if (MetaLog::Instance().FirstStream(id)) {
        MetaLog::Instance().Event("wt_pull", "openstream",
                                  sess.App(), sess.Stream(), sess.ExtraParams());
    }
    sess.StartMetaStats("wt_pull_stats", "stream_pull");
    auto writer = std::make_unique<WtFlvWriter>(sess, st);
    MediaStreamManager::AddPlayer(writer.get());
    pull_writers_[id] = std::move(writer);
}

} /* namespace cpp_streamer */
