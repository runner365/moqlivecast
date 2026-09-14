#include "event_log.hpp"
#include "utils/json.hpp"
#include <fstream>
#include <iomanip>
#include <iostream>
#include <chrono>
#include <sstream>

namespace cpp_streamer {

using json = nlohmann::json;

static std::string NowTimestamp() {
    using namespace std::chrono;
    auto now = system_clock::now();
    auto ms = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;
    std::time_t t = system_clock::to_time_t(now);
    std::tm tm;
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S") << ' ' << std::setw(3) << std::setfill('0') << ms.count();
    return oss.str();
}

EventLog::EventLog(const std::string& filename)
    : filename_(filename), stop_(false)
{
    worker_ = std::thread(&EventLog::WorkerLoop, this);
}

EventLog::~EventLog()
{
    stop_.store(true);
    cv_.notify_one();
    if (worker_.joinable()) worker_.join();
}

void EventLog::Log(const std::string& evt_name, json& json_data)
{
    try {
        std::lock_guard<std::mutex> lk(mutex_);
        queue_.emplace(evt_name, json_data);
        cv_.notify_one();
    } catch (...) {
        std::cout << "EventLog::Log exception" << std::endl;
    }
}

void EventLog::WorkerLoop()
{
    std::ofstream ofs;
    while (true) {
        std::unique_lock<std::mutex> lk(mutex_);
        cv_.wait(lk, [this]() { return stop_.load() || !queue_.empty(); });

        while (!queue_.empty()) {
            auto item = std::move(queue_.front());
            queue_.pop();
            lk.unlock();

            try {
                json out;
                out["name"] = item.first;
                out["data"] = item.second;
                out["date"] = NowTimestamp();

                if (!ofs.is_open()) {
                    ofs.open(filename_, std::ios::app | std::ios::binary);
                }
                if (ofs.is_open()) {
                    std::string line = out.dump();
                    ofs.write(line.c_str(), static_cast<std::streamsize>(line.size()));
                    ofs.write("\r\n", 2);
                    ofs.flush();
                }
            } catch (...) {
                // swallow errors for robustness
            }

            lk.lock();
        }

        if (stop_.load() && queue_.empty()) {
            break;
        }
    }

    // close file if open
    try {
        if (ofs.is_open()) ofs.close();
    } catch (...) {}
}

} // namespace cpp_streamer
