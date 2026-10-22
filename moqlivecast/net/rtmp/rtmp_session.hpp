#ifndef CO_RTMP_SESSION_HPP
#define CO_RTMP_SESSION_HPP
#ifdef _WIN64
#define WIN32_LEAN_AND_MEAN  // ���� Windows �ɰ�����ͷ�ļ������� winsock.h��
#endif
#include "utils/logger.hpp"
#include "utils/data_buffer.hpp"
#include "net/tcp/tcp_session.hpp"
#include "rtmp_pub.hpp"
#include "rtmp_handshake.hpp"
#include "rtmp_session_base.hpp"
#include "rtmp_control_handler.hpp"
#include "rtmp_request.hpp"
#include "media_packet.hpp"
#include "flv_catalog.hpp"
#include <memory>
#include <string>

namespace cpp_streamer
{
class RtmpWriter;
class RtmpServer;

class RtmpSession : public RtmpSessionBase, public TcpSessionCallbackI
{
public:
    RtmpSession(uv_loop_t* loop,
        uv_stream_t* server_uv_handle,
        RtmpServer* server,
        int64_t idle_timeout_ms,
        Logger* logger);
    virtual ~RtmpSession();

public:
	std::string GetId() { return id_; }
    void Close();

public:
    bool IsAlive();
    std::string GetSessonKey();

    /* publish 命令就绪时认领 stream key（owner = 本会话 id）。
     * 返回 false = 被顶替防抖拒绝，调用方应关闭会话。 */
    bool OnPublishReady() override;

public:
    int HandleRequest();

protected:
    virtual void OnWrite(int ret_code, size_t sent_size) override;
    virtual void OnRead(int ret_code, const char* data, size_t data_size) override;

public:
    virtual DataBuffer* GetRecvBuffer() override;
    virtual int RtmpSend(char* data, int len) override;
    virtual int RtmpSend(std::shared_ptr<DataBuffer> data_ptr) override;

private:
    int ReceiveChunkStream();

    /* 收到 AVC/AAC sequence header 时更新 moq-lite 的 catalog.json
     * （H.264 + AAC → codec/description/宽高/采样率）。
     *
     * ⚠️ 更新会**合并**：两条轨的 sequence header 紧挨着到（OBS/ffmpeg 都是
     * 这样），如果每来一条就发一次 catalog，订阅方会先看到“只有视频”的中间态。
     * moq-mux 的 FLV exporter 在文件头之后不允许已绑定的轨消失（会报
     * "FLV track removed mid-stream"），音频就会永久丢掉。
     * 所以：两条轨都到齐立即发；只有一条时等下一帧（最多 ~100ms）再发。 */
    void UpdateCatalogFromSeqHdr(const Media_Packet_Ptr& pkt);
    void FlushCatalog();
	
private:
    std::string id_;
    std::shared_ptr<TcpSession> conn_ptr_;
    RtmpServer* server_ = nullptr;   /* 用于顶替时踢掉旧的 RTMP 会话 */
    Logger* logger_ = nullptr;

private:
    RTMP_SERVER_SESSION_PHASE phase_ = initial_phase;
    bool running_ = false;
    bool closed_flag_ = false;

private:
    RtmpServerHandshake hs_;
    RtmpControlHandler control_handler_;

private:
    int64_t alive_ms_ = -1;
    /* 空闲超时（毫秒）：最后一次读或写之后超过它就关闭会话。
     * 由 RtmpServer 从 Config::RtmpIdleTimeoutMs() 传进来。 */
    int64_t idle_timeout_ms_ = 15000;
    /* publish 是否已经认领成功（ClaimPublisher）。只有 claimed_ 且仍是 owner
     * 的会话才允许把帧写进 MediaStreamManager。 */
    bool claimed_ = false;
    /* owner 校验丢弃日志的限流时间戳（秒）。 */
    int64_t owner_drop_warn_s_ = 0;
    /* 从 RTMP 的 FLV 序列头合成 moq-lite catalog（阶段 D）。 */
    FlvCatalogBuilder catalog_;
    /* catalog 合并窗：有 seq header 更新但还没发出去。两轨到齐立即 flush；
     * 只有一轨时由下一帧触发（见 UpdateCatalogFromSeqHdr）。 */
    bool catalog_pending_ = false;
    int64_t catalog_pending_ms_ = 0;

private:
    RtmpWriter* play_writer_ = nullptr;
};

}
#endif // CO_RTMP_SESSION_HPP