#ifndef MEDIA_OVER_WT_META_LOG_HPP
#define MEDIA_OVER_WT_META_LOG_HPP

#include "quic_connection.h"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <unordered_map>

namespace cpp_streamer {

class MetaLog {
public:
    static MetaLog &Instance();

    /* filename 为空则关闭。interval 已在 Config 里夹到 [2, 10] */
    void Init(const std::string &filename, int interval_sec);
    void Shutdown();
    bool Enabled() const { return enabled_; }
    int IntervalMs() const { return interval_sec_ * 1000; }

    void Event(const std::string &ev_name, const std::string &phase,
               const std::string &app, const std::string &stream,
               const std::string &params);

    void Stats(const std::string &ev_name, const std::string &phase,
               const std::string &app, const std::string &stream,
               const std::string &params,
               const QuicConnectionStats &st, bool push);

    /* 同一 session 首次开流返回 true */
    bool FirstStream(const std::string &id);
    void ForgetSession(const std::string &id);

    MetaLog(const MetaLog &) = delete;
    MetaLog &operator=(const MetaLog &) = delete;

private:
    MetaLog() = default;
    ~MetaLog();

    void Enqueue(std::string line);
    void WorkerLoop();

    bool enabled_ = false;
    int interval_sec_ = 3;
    std::string filename_;
    std::unordered_map<std::string, bool> stream_opened_;

    std::thread worker_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::queue<std::string> queue_;
    std::atomic<bool> stop_{true};
};

} /* namespace cpp_streamer */

#endif /* MEDIA_OVER_WT_META_LOG_HPP */
