#include "media_stream_manager.hpp"
#include "media_packet.hpp"
#include "logger.hpp"
#include <sstream>
#include <vector>

namespace cpp_streamer
{
    std::unordered_map<std::string, MEDIA_STREAM_PTR> MediaStreamManager::media_streams_map_;
    std::vector<StreamManagerCallbackI*> MediaStreamManager::cb_vec_;
    AvWriterInterface* MediaStreamManager::hls_writer_ = nullptr;
    AvWriterInterface* MediaStreamManager::r2r_writer_ = nullptr;

    PLAY_CALLBACK MediaStreamManager::play_cb_ = nullptr;
	Logger* MediaStreamManager::logger_ = nullptr;

    MediaStream::MediaStream(Logger* logger) :logger_(logger)
		, cache_(logger, 1) // default min_gop is 1
    {
    }
	MediaStream::~MediaStream() {
	}

    void MediaStream::NormalizeDts(Media_Packet_Ptr pkt) {
        if (!pkt) return;
        const int64_t dts = pkt->dts_ < 0 ? 0 : pkt->dts_;
        const int64_t pts = pkt->pts_ < 0 ? dts : pkt->pts_;
        const int64_t cts = pts - dts;

        if (clock_reset_pending_ && last_out_dts_ >= 0) {
            ts_shift_ = last_out_dts_ + 40 - dts;
            clock_reset_pending_ = false;
            last_in_dts_ = -1;
            LogInfof(logger_, "stream %s new publisher clock dts=%lld shift=%lld continue=%lld",
                     stream_key_.c_str(), (long long)dts, (long long)ts_shift_,
                     (long long)last_out_dts_);
        } else if (!pkt->is_seq_hdr_ && last_in_dts_ >= 0 && dts + 1000 < last_in_dts_) {
            ts_shift_ = (last_out_dts_ < 0 ? 0 : last_out_dts_) + 40 - dts;
            LogInfof(logger_, "stream %s dts reset %lld -> %lld, shift=%lld",
                     stream_key_.c_str(), (long long)last_in_dts_, (long long)dts,
                     (long long)ts_shift_);
        }
        clock_reset_pending_ = false;

        if (!pkt->is_seq_hdr_) last_in_dts_ = dts;

        pkt->dts_ = dts + ts_shift_;
        if (pkt->dts_ < 0) pkt->dts_ = 0;
        pkt->pts_ = pkt->dts_ + cts;
        if (pkt->pts_ < 0) pkt->pts_ = pkt->dts_;
        if (!pkt->is_seq_hdr_) last_out_dts_ = pkt->dts_;
    }

    bool MediaStreamManager::GetAppStreamname(const std::string& stream_key, std::string& app, std::string& streamname) {
        size_t pos = stream_key.find("/");
        if (pos == stream_key.npos) {
            return false;
        }
        app = stream_key.substr(0, pos);
        streamname = stream_key.substr(pos + 1);

        return true;
    }

    int MediaStreamManager::AddPlayer(AvWriterInterface* writer_p) {
        if (!writer_p) return 0;
        std::string key_str = writer_p->GetKey();
        std::string writerid = writer_p->GetWriterId();

        auto iter = media_streams_map_.find(key_str);
        if (iter == media_streams_map_.end()) {
            MEDIA_STREAM_PTR new_stream_ptr = std::make_shared<MediaStream>();
            new_stream_ptr->stream_key_ = key_str;
            new_stream_ptr->writer_map_[writerid] = writer_p;
            media_streams_map_.insert(std::make_pair(key_str, new_stream_ptr));
            LogInfof(logger_, "add player request:%s(%s) in new writer list",
                     key_str.c_str(), writerid.c_str());
            if (play_cb_) {
                play_cb_(key_str);
            }
            return 1;
        }

        MEDIA_STREAM_PTR stream_ptr = iter->second;
        LogInfof(logger_, "add player request:%s, stream_p:%p",
            key_str.c_str(), (void*)stream_ptr.get());
        /* 必须覆盖同 writerid：insert 失败会留下旧悬空指针，后续 live 全丢 */
        auto old = stream_ptr->writer_map_.find(writerid);
        if (old != stream_ptr->writer_map_.end() && old->second != writer_p) {
            LogWarnf(logger_, "add player replace stale writerid:%s key:%s",
                     writerid.c_str(), key_str.c_str());
        }
        stream_ptr->writer_map_[writerid] = writer_p;

        if (HasGopMedia(key_str) && !writer_p->IsInited()) {
            LogInfof(logger_, "add player replay gop key:%s hdr_v=%d hdr_a=%d pkts=%zu",
                     key_str.c_str(),
                     stream_ptr->cache_.VideoHdr() ? 1 : 0,
                     stream_ptr->cache_.AudioHdr() ? 1 : 0,
                     stream_ptr->cache_.Packets().size());
            writer_p->SetInitFlag(true);
            if (stream_ptr->cache_.WriterGop(writer_p) < 0) {
                LogWarnf(logger_, "add player gop write failed key:%s id:%s",
                         key_str.c_str(), writerid.c_str());
                writer_p->CloseWriter();
                RemovePlayer(writer_p);
                return 0;
            }
        }
        return (int)stream_ptr->writer_map_.size();
    }

    void MediaStreamManager::RemovePlayer(AvWriterInterface* writer_p) {
        std::string key_str = writer_p->GetKey();
        std::string writerid = writer_p->GetWriterId();

        LogInfof(logger_, "remove player key:%s", key_str.c_str());
        auto map_iter = media_streams_map_.find(key_str);
        if (map_iter == media_streams_map_.end()) {
            LogWarnf(logger_, "it's empty when remove player:%s", key_str.c_str());
            return;
        }

        auto writer_iter = map_iter->second->writer_map_.find(writerid);
        if (writer_iter != map_iter->second->writer_map_.end()) {
            if (writer_iter->second != writer_p) {
                LogWarnf(logger_, "remove player skip stale writerid:%s key:%s",
                         writerid.c_str(), key_str.c_str());
                return;
            }
            LogInfof(logger_, "remove player key:%s, erase writeid:%s", key_str.c_str(), writerid.c_str());
            map_iter->second->writer_map_.erase(writer_iter);
        }
        else {
            LogInfof(logger_, "remove player key:%s, fail to find writeid:%s, writer map size:%lu",
                key_str.c_str(), writerid.c_str(), map_iter->second->writer_map_.size());
        }

        if (map_iter->second->writer_map_.empty() && !map_iter->second->publisher_exist_) {
            //playlist is empty and the publisher does not exist
            media_streams_map_.erase(map_iter);
            LogInfof(logger_, "delete stream %s for the publisher and players are empty.", key_str.c_str());
        }
        return;
    }

    MEDIA_STREAM_PTR MediaStreamManager::AddPublisher(const std::string& stream_key) {
        MEDIA_STREAM_PTR ret_stream_ptr;

        auto iter = media_streams_map_.find(stream_key);
        if (iter == media_streams_map_.end()) {
            ret_stream_ptr = std::make_shared<MediaStream>();
            ret_stream_ptr->publisher_exist_ = true;
            ret_stream_ptr->stream_key_ = stream_key;
            LogInfof(logger_, "add new publisher stream key:%s, stream_p:%p",
                stream_key.c_str(), (void*)ret_stream_ptr.get());
            media_streams_map_.insert(std::make_pair(stream_key, ret_stream_ptr));

            std::string app;
            std::string streamname;
            if (GetAppStreamname(stream_key, app, streamname)) {
                for (auto cb : cb_vec_) {
                    cb->OnPublish(app, streamname);
                }
            }

            return ret_stream_ptr;
        }
        ret_stream_ptr = iter->second;
        ret_stream_ptr->publisher_exist_ = true;
        return ret_stream_ptr;
    }

    void MediaStreamManager::RemovePublisher(const std::string& stream_key) {
        auto iter = media_streams_map_.find(stream_key);
        if (iter == media_streams_map_.end()) {
            LogWarnf(logger_, "There is not publish key:%s", stream_key.c_str());
            return;
        }

        LogInfof(logger_, "remove publisher in media stream:%s", stream_key.c_str());
        iter->second->publisher_exist_ = false;
        iter->second->clock_reset_pending_ = true;
        iter->second->cache_.Clear();
        LogInfof(logger_, "clear gop after unpublish key:%s", stream_key.c_str());
        if (iter->second->writer_map_.empty()) {
            LogInfof(logger_, "delete stream %s for the publisher and players are empty.", stream_key.c_str());
            media_streams_map_.erase(iter);
        }

        std::string app;
        std::string streamname;
        if (GetAppStreamname(stream_key, app, streamname)) {
            for (auto cb : cb_vec_) {
                cb->OnUnpublish(app, streamname);
            }
        }
        return;
    }

    void MediaStreamManager::SetHlsWriter(AvWriterInterface* writer) {
        hls_writer_ = writer;
    }

    void MediaStreamManager::SetRtcWriter(AvWriterInterface* writer) {
        r2r_writer_ = writer;
    }

    PLAY_CALLBACK MediaStreamManager::GetPlayCallback() {
        return play_cb_;
    }

    void MediaStreamManager::SetPlayCallback(PLAY_CALLBACK cb) {
        play_cb_ = cb;
    }

    GopCache *MediaStreamManager::GetGop(const std::string &stream_key) {
        auto iter = media_streams_map_.find(stream_key);
        if (iter == media_streams_map_.end() || !iter->second) return nullptr;
        return &iter->second->cache_;
    }

    bool MediaStreamManager::HasGopMedia(const std::string &stream_key) {
        GopCache *gop = GetGop(stream_key);
        if (!gop) return false;
        if (gop->VideoHdr() || gop->AudioHdr()) return true;
        return !gop->Packets().empty();
    }

    int MediaStreamManager::WriterMediaPacket(Media_Packet_Ptr pkt_ptr) {
        if (!pkt_ptr) return -1;
        if (pkt_ptr->key_.empty() && !pkt_ptr->app_.empty() && !pkt_ptr->streamname_.empty()) {
            pkt_ptr->key_ = pkt_ptr->app_ + "/" + pkt_ptr->streamname_;
        }
        MEDIA_STREAM_PTR stream_ptr = AddPublisher(pkt_ptr->key_);
        int player_cnt = 0;

        if (!stream_ptr) {
            LogErrorf(logger_, "fail to get stream key:%s", pkt_ptr->key_.c_str());
            return -1;
        }

        stream_ptr->NormalizeDts(pkt_ptr);
        stream_ptr->cache_.InsertPacket(pkt_ptr);
        std::vector<AvWriterInterface*> remove_list;

        for (auto item : stream_ptr->writer_map_) {
            auto writer = item.second;
            if (!writer->IsInited()) {
                writer->SetInitFlag(true);
                if (stream_ptr->cache_.WriterGop(writer) < 0) {
                    remove_list.push_back(writer);
                }
                else {
                    player_cnt++;
                }
            }
            else {
                if (writer->WritePacket(pkt_ptr) < 0) {
                    std::string key_str = writer->GetKey();
                    std::string writerid = writer->GetWriterId();
                    LogWarnf(logger_, "writer send packet error, key:%s, id:%s",
                        key_str.c_str(), writerid.c_str());
                    remove_list.push_back(writer);
                }
                else {
                    player_cnt++;
                }
            }
        }

        if (MediaStreamManager::r2r_writer_) {
            Media_Packet_Ptr new_pkt_ptr = pkt_ptr->copy();
            MediaStreamManager::r2r_writer_->WritePacket(new_pkt_ptr);
        }

        if (MediaStreamManager::hls_writer_) {
            Media_Packet_Ptr new_pkt_ptr = pkt_ptr->copy();
            MediaStreamManager::hls_writer_->WritePacket(new_pkt_ptr);
        }

        for (auto write_p : remove_list) {
            RemovePlayer(write_p);
        }

        return player_cnt;
    }
    void MediaStreamManager::SetLogger(Logger* logger) {
        logger_ = logger;
    }
	Logger* MediaStreamManager::GetLogger() {
		return logger_;
	}
}