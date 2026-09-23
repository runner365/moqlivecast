#ifndef WT_SERVER_SESSION_HPP
#define WT_SERVER_SESSION_HPP

#include "webtransport_server_api.h"
#include "wt_server/wt_server_stream.hpp"
#include "utils/timer.hpp"

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

namespace cpp_streamer {

class WTServerSession {
public:
    using OnStreamOpen = std::function<void(WTServerSession &sess,
                                            WTServerStream &st)>;

    WTServerSession(wt_session_t *sess, std::string route_path);
    ~WTServerSession();

    WTServerSession(const WTServerSession &) = delete;
    WTServerSession &operator=(const WTServerSession &) = delete;

    const std::string &RoutePath() const { return route_path_; }

    std::string Path() const;
    std::string GetParam(const std::string &name) const;
    /* query 里除 method/app/stream 外的 k=v&k=v */
    std::string ExtraParams() const;

    /* URL：/flv?method=push|pull&app=live&stream=123456 */
    const std::string &Method() const { return method_; }
    const std::string &App() const { return app_; }
    const std::string &Stream() const { return stream_; }
    const std::string &SessionId() const { return session_id_; }
    /* 同一路媒体的 key，如 live/123456，push/pull 共用 */
    const std::string &MediaPath() const { return media_path_; }

    void SetPublish(const std::string &app, const std::string &stream) {
        method_ = "push";
        app_ = app;
        stream_ = stream;
        media_path_ = app_ + "/" + stream_;
    }

    void SetSubscribe(const std::string &app, const std::string &stream) {
        method_ = "pull";
        app_ = app;
        stream_ = stream;
        media_path_ = app_ + "/" + stream_;
    }

    void SetAppData(void *data) { app_data_ = data; }
    void *AppData() const { return app_data_; }

    void Close();
    void OpenStream(OnStreamOpen cb);
    /* 打开一条【本端发起的单向流】。MOQ 规范要求控制流是一对单向流：
     * 对端那条用于发请求，本端这条用于回应答（如 SUBSCRIBE_OK）。
     * 单向流只能写，不会收到 on_stream_data。
     * 返回新流；失败（如尚无 MAX_STREAMS_UNI 配额）返回 nullptr。 */
    WTServerStream *OpenUniStream();

    /* 底层 QUIC 路径 / 拥塞快照 */
    bool GetQuicStats(QuicConnectionStats *out) const;

    /* 会话内定时写 stream stats；析构 / Detach 时自动停 */
    void StartMetaStats(const std::string &stats_ev, const std::string &phase);
    void StopMetaStats();

    WTServerStream *EnsureStream(wt_stream_t *st);
    void RemoveStream(wt_stream_t *st);
    void ForEachStream(const std::function<void(WTServerStream &st)> &fn);

    wt_session_t *Handle() const { return sess_; }
    bool Valid() const { return sess_ != nullptr; }

    static WTServerSession *From(wt_session_t *sess);

    void Detach();

private:
    static void OnStreamOpenTrampoline(wt_server_t *srv, wt_session_t *sess,
                                       wt_stream_t *st, void *user);

    wt_session_t *sess_ = nullptr;
    std::string route_path_;
    std::string method_;
    std::string app_;
    std::string stream_;
    std::string session_id_;
    std::string media_path_;
    void *app_data_ = nullptr;
    OnStreamOpen pending_open_cb_;
    std::unordered_map<wt_stream_t *, std::unique_ptr<WTServerStream>> streams_;
    std::unique_ptr<TimerInterface> meta_stats_;
};

} /* namespace cpp_streamer */

#endif /* WT_SERVER_SESSION_HPP */
