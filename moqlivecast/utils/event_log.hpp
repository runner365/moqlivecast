#ifndef EVENT_LOG_HPP
#define EVENT_LOG_HPP
#include "utils/json.hpp"
#include <string>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <atomic>

namespace cpp_streamer
{
class EventLog
{
public:
    EventLog(const std::string& filename);
    ~EventLog();

    // Log an event: name and JSON data. This method is thread-safe and returns quickly.
    void Log(const std::string& evt_name, nlohmann::json& json_data);

    // non-copyable
    EventLog(const EventLog&) = delete;
    EventLog& operator=(const EventLog&) = delete;

private:
    void WorkerLoop();

    std::string filename_;
    std::thread worker_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::queue<std::pair<std::string, nlohmann::json>> queue_;
    std::atomic<bool> stop_{false};
};
}

#endif//EVENT_LOG_HPP