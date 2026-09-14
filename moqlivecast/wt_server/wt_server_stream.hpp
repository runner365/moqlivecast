#ifndef WT_SERVER_STREAM_HPP
#define WT_SERVER_STREAM_HPP

#include "webtransport_server_api.h"

#include <cstdint>
#include <functional>

namespace cpp_streamer {

class WTServer;
class WTServerSession;

/* 服务端侧一条 WT stream：当前只负责接收客户端 push 的数据。
 * C 层没有 pull-read，字节由 on_stream_data 推入 Feed → OnData。
 * 媒体解封装 / 转推后续再接，不要在这里处理 payload。 */
class WTServerStream {
public:
    using OnData = std::function<void(WTServerStream &st,
                                      const uint8_t *data, size_t len)>;
    using OnWritable = std::function<void(WTServerStream &st)>;

    WTServerStream(wt_stream_t *st, WTServerSession *sess);
    ~WTServerStream();

    WTServerStream(const WTServerStream &) = delete;
    WTServerStream &operator=(const WTServerStream &) = delete;

    /* 这条流上每次收到客户端数据时调用；不设则只走路径级 OnStreamData */
    void SetOnData(OnData cb) { on_data_ = std::move(cb); }
    void SetOnWritable(OnWritable cb);

    /* 0 已接受；1 拥塞（未接受）；-1 异常 */
    int Write(const uint8_t *data, size_t len);

    void SetAppData(void *data) { app_data_ = data; }
    void *AppData() const { return app_data_; }

    WTServerSession *Session() const { return sess_; }
    wt_stream_t *Handle() const { return st_; }
    bool Valid() const { return st_ != nullptr; }

    static WTServerStream *From(wt_stream_t *st);

    void Detach();

private:
    friend class WTServer;
    friend class WTServerSession;

    void Feed(const uint8_t *data, size_t len);
    static void OnWritableTrampoline(wt_stream_t *st, void *user);

    wt_stream_t *st_ = nullptr;
    WTServerSession *sess_ = nullptr;
    void *app_data_ = nullptr;
    OnData on_data_;
    OnWritable on_writable_;
};

} /* namespace cpp_streamer */

#endif /* WT_SERVER_STREAM_HPP */
