#include "utils/meta_log.hpp"
#include "utils/json.hpp"
#include "logger.h"

#include <ctime>
#include <fstream>
#include <utility>

namespace cpp_streamer {

using json = nlohmann::json;

static uint64_t UnixSec() {
    return static_cast<uint64_t>(std::time(nullptr));
}

MetaLog &MetaLog::Instance() {
    static MetaLog inst;
    return inst;
}

MetaLog::~MetaLog() {
    Shutdown();
}

void MetaLog::Init(const std::string &filename, int interval_sec) {
    Shutdown();
    filename_ = filename;
    interval_sec_ = interval_sec;
    if (interval_sec_ < 2) interval_sec_ = 2;
    if (interval_sec_ > 10) interval_sec_ = 10;
    enabled_ = !filename_.empty();
    if (!enabled_) {
        LOG_INFO("[meta_log] disabled (empty path)");
        return;
    }
    stop_.store(false);
    worker_ = std::thread(&MetaLog::WorkerLoop, this);
    LOG_INFO("[meta_log] file=%s interval=%ds",
             filename_.c_str(), interval_sec_);
}

void MetaLog::Shutdown() {
    if (!stop_.exchange(true)) {
        cv_.notify_one();
        if (worker_.joinable()) worker_.join();
    }
    stream_opened_.clear();
    enabled_ = false;
}

void MetaLog::Enqueue(std::string line) {
    if (!enabled_ || line.empty()) return;
    try {
        std::lock_guard<std::mutex> lk(mutex_);
        queue_.push(std::move(line));
        cv_.notify_one();
    } catch (...) {
    }
}

void MetaLog::Event(const std::string &ev_name, const std::string &phase,
                    const std::string &app, const std::string &stream,
                    const std::string &params) {
    if (!enabled_) return;
    json j;
    j["ts"] = UnixSec();
    j["ev_name"] = ev_name;
    j["phase"] = phase;
    j["app"] = app;
    j["stream"] = stream;
    j["params"] = params;
    Enqueue(j.dump());
}

void MetaLog::Stats(const std::string &ev_name, const std::string &phase,
                    const std::string &app, const std::string &stream,
                    const std::string &params,
                    const QuicConnectionStats &st, bool push) {
    if (!enabled_) return;
    json j;
    j["ts"] = UnixSec();
    j["ev_name"] = ev_name;
    j["phase"] = phase;
    j["app"] = app;
    j["stream"] = stream;
    j["params"] = params;
    j["kbps"] = push ? st.recv_kbps : st.send_kbps;
    j["send_kbps"] = st.send_kbps;
    j["recv_kbps"] = st.recv_kbps;
    j["rtt"] = st.srtt_ms;
    j["latest_rtt"] = st.latest_rtt_ms;
    j["min_rtt"] = st.min_rtt_ms;
    j["jitter"] = st.jitter_ms;
    j["inflight"] = st.bytes_in_flight;
    j["cwnd"] = st.cwnd;
    j["ssthresh"] = st.ssthresh;
    j["max_bw_kbps"] = st.max_bw_kbps;
    j["packets_lost"] = st.packets_lost;
    j["packets_acked"] = st.packets_acked;
    j["pto"] = st.pto_ms;
    Enqueue(j.dump());
}

bool MetaLog::FirstStream(const std::string &id) {
    if (id.empty()) return false;
    if (stream_opened_[id]) return false;
    stream_opened_[id] = true;
    return true;
}

void MetaLog::ForgetSession(const std::string &id) {
    stream_opened_.erase(id);
}

void MetaLog::WorkerLoop() {
    std::ofstream ofs;
    while (true) {
        std::unique_lock<std::mutex> lk(mutex_);
        cv_.wait(lk, [this]() { return stop_.load() || !queue_.empty(); });
        while (!queue_.empty()) {
            std::string line = std::move(queue_.front());
            queue_.pop();
            lk.unlock();
            try {
                if (!ofs.is_open()) {
                    ofs.open(filename_, std::ios::app | std::ios::binary);
                }
                if (ofs.is_open()) {
                    ofs.write(line.c_str(), static_cast<std::streamsize>(line.size()));
                    ofs.write("\n", 1);
                    ofs.flush();
                }
            } catch (...) {
            }
            lk.lock();
        }
        if (stop_.load() && queue_.empty()) break;
    }
    if (ofs.is_open()) ofs.close();
}

} /* namespace cpp_streamer */
