#include "media_stream_manager.hpp"
#include "media_packet.hpp"
#include "logger.hpp"
#include <chrono>
#include <sstream>
#include <vector>

namespace cpp_streamer
{

    namespace {

    /* 顶替防抖：同一个 key 在窗口内被顶超过 kMaxTakeOverPerWindow 次就拒新。
     * 目的只是拦住「两个真实推流端撞名 / 客户端断了就自动重连」这种互相踢的
     * 死循环；正常的重启（旧的已经干净摘牌）走不到这里，因为那时
     * publisher_id_ 已被清空，claim 结果是 kReused。 */
    constexpr int64_t kTakeOverWindowMs = 10000;
    constexpr int kMaxTakeOverPerWindow = 3;

    int64_t NowMs() {
        return static_cast<int64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
    }

    } /* namespace */

    std::unordered_map<std::string, MEDIA_STREAM_PTR> MediaStreamManager::media_streams_map_;
    std::vector<StreamManagerCallbackI*> MediaStreamManager::cb_vec_;
    std::vector<MediaStreamManager::CatalogUpdatedFn> MediaStreamManager::catalog_updated_cbs_;
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

        /* 回退基线**按轨**取：跨轨比较会把两条时钟的 epoch 差当成回退。 */
        int64_t &last_in = (pkt->av_type_ == MEDIA_VIDEO_TYPE) ? last_in_video_dts_
                         : (pkt->av_type_ == MEDIA_AUDIO_TYPE) ? last_in_audio_dts_
                         : last_in_dts_;

        if (clock_reset_pending_ && last_out_dts_ >= 0) {
            ts_shift_ = last_out_dts_ + 40 - dts;
            clock_reset_pending_ = false;
            last_in_video_dts_ = last_in_audio_dts_ = last_in_dts_ = -1;
            LogInfof(logger_, "stream %s new publisher clock dts=%lld shift=%lld continue=%lld",
                     stream_key_.c_str(), (long long)dts, (long long)ts_shift_,
                     (long long)last_out_dts_);
        } else if (!pkt->is_seq_hdr_ && last_in >= 0 && dts + 1000 < last_in) {
            ts_shift_ = (last_out_dts_ < 0 ? 0 : last_out_dts_) + 40 - dts;
            LogInfof(logger_, "stream %s track %s dts reset %lld -> %lld, shift=%lld",
                     stream_key_.c_str(),
                     pkt->av_type_ == MEDIA_VIDEO_TYPE ? "video" : "audio",
                     (long long)last_in, (long long)dts, (long long)ts_shift_);
            /* 这个 shift 作用在**共享**输出时间轴上，另一条轨的基线也必须清掉，
             * 否则它下个包会再触发一次「回退」，把时间轴再推 40ms。 */
            last_in_video_dts_ = last_in_audio_dts_ = last_in_dts_ = -1;
        }
        clock_reset_pending_ = false;

        if (!pkt->is_seq_hdr_) last_in = dts;

        /* ── 诊断：两条原始时间轴的 epoch 差 ──
         * 正常 < 几十 ms；一旦差出秒级，说明发布端的两个时钟不同源。
         * 旧逻辑会在每次交替时「修正」它，结果是把时间轴加速成倍的灾难；
         * 现在不改行为，但要让它**可见**。 */
        if (last_in_video_dts_ >= 0 && last_in_audio_dts_ >= 0) {
            const int64_t skew = last_in_video_dts_ - last_in_audio_dts_;
            const int64_t now_s = NowMs() / 1000;
            if ((skew > 1000 || skew < -1000) && now_s != skew_warn_s_) {
                skew_warn_s_ = now_s;
                LogWarnf(logger_,
                         "stream %s audio/video timestamps differ by %lld ms "
                         "(video=%lld audio=%lld) — the publisher is using two "
                         "different clocks; A/V sync will be wrong",
                         stream_key_.c_str(), (long long)skew,
                         (long long)last_in_video_dts_,
                         (long long)last_in_audio_dts_);
            }
        }

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

        /* 没有 publisher 的 key 上来了播放器 = 本地缺流，可能是回源的机会。
         * 触发点在**写入 writer_map_ 之后**：回调里启动的回源/推流不会丢。
         * 新 key 的分支上面已经触发过一次，这里只覆盖「已有空壳」的情况。 */
        if (!stream_ptr->publisher_exist_ && play_cb_) {
            play_cb_(key_str);
        }

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
        iter->second->publisher_id_.clear();
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

    PublishClaimResult MediaStreamManager::ClaimPublisher(const std::string& stream_key,
                                                          const std::string& owner_id) {
        PublishClaimResult ret;

        auto iter = media_streams_map_.find(stream_key);
        if (iter == media_streams_map_.end()) {
            MEDIA_STREAM_PTR stream_ptr = std::make_shared<MediaStream>();
            stream_ptr->publisher_exist_ = true;
            stream_ptr->publisher_id_ = owner_id;
            stream_ptr->stream_key_ = stream_key;
            LogInfof(logger_, "add new publisher stream key:%s, owner:%s, stream_p:%p",
                stream_key.c_str(), owner_id.c_str(), (void*)stream_ptr.get());
            media_streams_map_.insert(std::make_pair(stream_key, stream_ptr));

            std::string app;
            std::string streamname;
            if (GetAppStreamname(stream_key, app, streamname)) {
                for (auto cb : cb_vec_) {
                    cb->OnPublish(app, streamname);
                }
            }

            ret.action = PublishClaim::kCreated;
            return ret;
        }

        MEDIA_STREAM_PTR stream_ptr = iter->second;
        /* 同一个会话重复 PUBLISH（catalog/video/audio 各一条），或者流上还没
         * 登记过 owner（老路径/媒体先到），都按复用处理，不触发顶替。 */
        if (stream_ptr->publisher_id_.empty() || stream_ptr->publisher_id_ == owner_id) {
            stream_ptr->publisher_exist_ = true;
            if (stream_ptr->publisher_id_.empty()) {
                stream_ptr->publisher_id_ = owner_id;
            }
            ret.action = PublishClaim::kReused;
            return ret;
        }

        const int64_t now = NowMs();
        if (stream_ptr->claim_window_ms_ != 0
            && now - stream_ptr->claim_window_ms_ <= kTakeOverWindowMs) {
            stream_ptr->claim_takeovers_++;
            if (stream_ptr->claim_takeovers_ > kMaxTakeOverPerWindow) {
                LogErrorf(logger_, "publisher take over rejected key:%s current:%s new:%s"
                    " takeovers:%d in %lldms",
                    stream_key.c_str(), stream_ptr->publisher_id_.c_str(),
                    owner_id.c_str(), stream_ptr->claim_takeovers_,
                    (long long)(now - stream_ptr->claim_window_ms_));
                ret.action = PublishClaim::kRejected;
                ret.previous_owner = stream_ptr->publisher_id_;
                return ret;
            }
        }
        else {
            stream_ptr->claim_window_ms_ = now;
            stream_ptr->claim_takeovers_ = 1;
        }

        /* 新顶旧：先把 owner 换成新的，调用方随后去断旧会话。
         * 顺序不能反 —— 被顶掉的会话关闭时会走到 ReleasePublisher，那时 owner
         * 已经不是它，才不会把新 publisher 刚建立的 cache 清掉。 */
        ret.action = PublishClaim::kTakeOver;
        ret.previous_owner = stream_ptr->publisher_id_;
        LogWarnf(logger_, "publisher take over key:%s old:%s new:%s takeovers:%d",
            stream_key.c_str(), ret.previous_owner.c_str(), owner_id.c_str(),
            stream_ptr->claim_takeovers_);
        stream_ptr->publisher_id_ = owner_id;
        stream_ptr->publisher_exist_ = true;
        /* 换源：旧 GOP 属于旧发布端（可能是另一个编码器实例 / 另一条时间轴）。
         * 新订阅者若回放它，第一帧之后会跟新源的 live 帧接不上 —— 实测表现为
         * 「出第一帧后卡住」。清掉缓存，等新源的关键帧重新开 GOP；现有播放器
         * 不受影响（它们是 live 扇出，不读缓存）。 */
        stream_ptr->cache_.Clear();
        return ret;
    }

    bool MediaStreamManager::IsPublisherOwner(const std::string& stream_key,
                                              const std::string& owner_id) {
        auto iter = media_streams_map_.find(stream_key);
        if (iter == media_streams_map_.end()) {
            return false;
        }
        /* owner 为空 = 这条流没登记过来源（RTMP 等老路径不会登记）。此时不能
         * 判定「被顶掉」：老路径的 RemovePublisher 会把 owner 清空，若在这里
         * 返回 false，还在正常推流的 moq 会话会被永久丢包。 */
        if (iter->second->publisher_id_.empty()) {
            return true;
        }
        return iter->second->publisher_id_ == owner_id;
    }

    bool MediaStreamManager::ReleasePublisher(const std::string& stream_key,
                                              const std::string& owner_id) {
        auto iter = media_streams_map_.find(stream_key);
        if (iter == media_streams_map_.end()) {
            return false;
        }
        /* 同上：没登记 owner 的流按老行为直接摘牌，否则 publisher_exist_ 会
         * 一直挂着，流要等到最后一个 player 离开才回收。 */
        if (!iter->second->publisher_id_.empty()
            && iter->second->publisher_id_ != owner_id) {
            LogWarnf(logger_, "skip stale publisher release key:%s owner:%s current:%s",
                stream_key.c_str(), owner_id.c_str(),
                iter->second->publisher_id_.c_str());
            return false;
        }
        RemovePublisher(stream_key);
        return true;
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

    bool MediaStreamManager::HasPublisher(const std::string &stream_key) {
        auto iter = media_streams_map_.find(stream_key);
        return iter != media_streams_map_.end() && iter->second &&
               iter->second->publisher_exist_;
    }

    size_t MediaStreamManager::PlayerCount(const std::string &stream_key) {
        auto iter = media_streams_map_.find(stream_key);
        if (iter == media_streams_map_.end() || !iter->second) return 0;
        return iter->second->writer_map_.size();
    }

    std::vector<std::string> MediaStreamManager::ListPublisherKeys() {
        std::vector<std::string> keys;
        /* 单线程假设：整个进程只有一个 uv loop（moq.cpp 的 uv_run），
         * WriterMediaPacket 的扇出与 QUIC 回调都跑在它上面，所以这里不加锁
         * —— 与 MoqPushSession 从别的连接线程直接写 QUIC 是同一套前提。 */
        keys.reserve(media_streams_map_.size());
        for (const auto &item : media_streams_map_) {
            if (item.second && item.second->publisher_exist_) {
                keys.push_back(item.first);
            }
        }
        return keys;
    }

    bool MediaStreamManager::SetCatalogJson(const std::string &stream_key,
                                            const std::string &owner_id,
                                            const std::string &json) {
        if (json.empty()) return false;

        auto iter = media_streams_map_.find(stream_key);
        if (iter == media_streams_map_.end() || !iter->second) return false;

        /* 被顶掉的旧会话仍在途中的 catalog 不许覆盖新 publisher 的 —— 判据与
         * IsPublisherOwner 完全一致（owner 为空的老路径不做校验）。 */
        if (!iter->second->publisher_id_.empty()
            && iter->second->publisher_id_ != owner_id) {
            LogWarnf(logger_, "skip stale catalog key:%s owner:%s current:%s",
                     stream_key.c_str(), owner_id.c_str(),
                     iter->second->publisher_id_.c_str());
            return false;
        }
        if (iter->second->catalog_json_ == json) return true;

        iter->second->catalog_json_ = json;
        iter->second->catalog_rev_++;
        LogInfof(logger_, "catalog stored key:%s bytes:%zu rev:%llu",
                 stream_key.c_str(), json.size(),
                 (unsigned long long)iter->second->catalog_rev_);

        /* 通知订阅侧补发新的一组。回调可能写 QUIC（同一条 loop），所以放在
         * 存储完成之后；回调用改回 SetCatalogJson 会导致递归，调用方自己保证。
         * 用**拷贝**遍历：回调里注册/注销监听者（例如 cluster 收尾）不能让
         * 这里拿着失效的迭代器。顺序 = 注册顺序（moq-lite 先于 cluster）。 */
        const auto cbs = catalog_updated_cbs_;
        const uint64_t rev = iter->second->catalog_rev_;
        for (const auto &cb : cbs) {
            if (cb) cb(stream_key, rev);
        }
        return true;
    }

    bool MediaStreamManager::GetCatalogJson(const std::string &stream_key,
                                            std::string &out, uint64_t *rev) {
        auto iter = media_streams_map_.find(stream_key);
        if (iter == media_streams_map_.end() || !iter->second) return false;
        if (iter->second->catalog_json_.empty()) return false;
        out = iter->second->catalog_json_;
        if (rev) *rev = iter->second->catalog_rev_;
        return true;
    }

    void MediaStreamManager::SetCatalogUpdatedCallback(CatalogUpdatedFn fn) {
        catalog_updated_cbs_.clear();
        if (fn) catalog_updated_cbs_.push_back(std::move(fn));
    }

    void MediaStreamManager::AddCatalogUpdatedCallback(CatalogUpdatedFn fn) {
        if (fn) catalog_updated_cbs_.push_back(std::move(fn));
    }

    void MediaStreamManager::ClearCatalogUpdatedCallbacks() {
        catalog_updated_cbs_.clear();
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