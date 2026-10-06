#ifndef RTMP_MEDIA_STREAM_HPP
#define RTMP_MEDIA_STREAM_HPP
#ifdef _WIN64
#define WIN32_LEAN_AND_MEAN  // ���� Windows �?�����?�?������� winsock.h��
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
        /* 当前 publisher 的会话 id。空 = 没登记来源（RTMP 等老路径不填），
         * 此时不做 owner 校验，保持旧行为。 */
        std::string publisher_id_;
        /* 顶替防抖：窗口起点(ms) + 窗口内已被顶的次数，见 ClaimPublisher。 */
        int64_t claim_window_ms_ = 0;
        int claim_takeovers_ = 0;
        int64_t last_in_dts_ = -1;
        int64_t last_out_dts_ = -1;
        int64_t ts_shift_ = 0;
        GopCache cache_;
        WRITER_MAP writer_map_;//(session_key, av_writer_base*)

        /* catalog.json 的**原文**（推流侧解出并校验通过的那份），逐字节保存。
         * 发布端角色（moq-lite export）要原样发出去 —— 从 FLV 序列头反推
         * 不出 codec 串（avc1.PPCCLL / mp4a.40.2）和 clock，而 export flv
         * 两样都依赖。空 = 这条流没有 catalog，**不可对外发布**。 */
        std::string catalog_json_;
        /* 每次 catalog 更新自增。订阅方据此判断要不要重发（重发一次就够，
         * 不必每帧都带）。 */
        uint64_t catalog_rev_ = 0;

        void NormalizeDts(Media_Packet_Ptr pkt);
    };

    typedef std::shared_ptr<MediaStream> MEDIA_STREAM_PTR;

    /* 一个 Full Track Name 同时只能有一个 publisher（draft-ietf-moq-transport
     * §2.4.3：同一 scope 内同名 track 的内容必须唯一）。因此同一个
     * app/stream 再来一个不同会话的 PUBLISH 时，按「新顶旧」处理：新会话成为
     * publisher，旧会话由调用方断开（见 ClaimPublisher 的返回值）。 */
    enum class PublishClaim {
        kCreated,   /* 新建流，直接成为 publisher */
        kReused,    /* 同一个 owner 重复 PUBLISH（catalog/video/audio 三条），
                     * 或者流上还没登记过 owner */
        kTakeOver,  /* 顶掉了另一个 owner，previous_owner 是被顶掉的会话 id */
        kRejected,  /* 防抖命中：窗口内被顶太频繁，拒绝本次（调用方应断开新会话） */
    };

    struct PublishClaimResult {
        PublishClaim action = PublishClaim::kCreated;
        std::string previous_owner;
    };

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

        /* PUBLISH 到达时认领 stream_key：owner_id 是会话 id。
         * 与 AddPublisher 的区别是它带 owner、会做顶替与防抖。 */
        static PublishClaimResult ClaimPublisher(const std::string& stream_key,
                                                 const std::string& owner_id);
        /* 当前 publisher 是不是这个 owner。被顶掉之后旧 owner 就返回 false，
         * 媒体路径据此丢弃旧会话还在途中的对象，避免两条时间线混进一条流。 */
        static bool IsPublisherOwner(const std::string& stream_key,
                                     const std::string& owner_id);
        /* 只有 owner 自己能摘牌。被顶掉的旧会话在关闭时也会走到这里，
         * 不校验就会把新 publisher 刚建立的 cache 清掉。 */
        static bool ReleasePublisher(const std::string& stream_key,
                                     const std::string& owner_id);

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

        /* 当前登记为「有 publisher」的 stream_key 清单，供发布端角色
         * （moq-lite export）回答 ANNOUNCE 用。
         * 返回**拷贝**而不是内部引用：调用方在 QUIC 回调里遍历，而对端随时
         * 可能 PUBLISH/UNPUBLISH，拿着引用跨出这次调用就是悬垂。
         * 按 publisher_exist_ 过滤（不是 publisher_id_），这样 RTMP 那条
         * 老路径的流也在内 —— 它们填不上 catalog，由调用方按 catalog_json_
         * 是否为空再过滤一层，见 SetCatalogJson 的说明。 */
        static std::vector<std::string> ListPublisherKeys();

        /* catalog.json 的原文存取。owner_id 把关，语义与 IsPublisherOwner
         * 一致：被顶掉的旧会话不许覆盖新 publisher 的 catalog。 */
        static bool SetCatalogJson(const std::string &stream_key,
                                   const std::string &owner_id,
                                   const std::string &json);
        static bool GetCatalogJson(const std::string &stream_key, std::string &out);

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