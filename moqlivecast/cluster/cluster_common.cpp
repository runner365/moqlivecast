#include "cluster_common.hpp"

#include "utils/timeex.hpp"

#include <cstddef>

namespace cpp_streamer {

std::string CenterUrl(const ClusterOptions &opts) {
    std::string path = opts.center_path;
    while (!path.empty() && path[0] == '/') path.erase(path.begin());
    std::string url = "moql://" + opts.center_host + ":" +
                      std::to_string(opts.center_port);
    if (!path.empty()) url += "/" + path;
    return url;
}

bool SplitLocalKey(const std::string &local_key, std::string *app,
                   std::string *stream) {
    const size_t pos = local_key.find('/');
    if (pos == std::string::npos || pos == 0 || pos + 1 >= local_key.size()) {
        return false;
    }
    if (app) *app = local_key.substr(0, pos);
    if (stream) *stream = local_key.substr(pos + 1);
    return true;
}

bool WireBroadcastOf(const std::string &local_key, std::string *wire) {
    std::string stream;
    if (!SplitLocalKey(local_key, nullptr, &stream)) return false;
    if (stream.empty()) return false;
    if (wire) *wire = stream;
    return true;
}

int64_t ClusterNowMs() {
    return now_millisec();
}

int ClusterBackoffMs(const ClusterOptions &opts, int attempt,
                     const std::string &key, int64_t now_ms) {
    int min_ms = opts.reconnect_min_ms > 0 ? opts.reconnect_min_ms : 500;
    int max_ms = opts.reconnect_max_ms >= min_ms ? opts.reconnect_max_ms
                                                 : min_ms;
    int64_t delay = min_ms;
    for (int i = 0; i < attempt && delay < max_ms; i++) delay *= 2;
    if (delay > max_ms) delay = max_ms;
    if (delay < min_ms) delay = min_ms;

    /* ±20% 的确定性抖动：同 key 的多次重连之间也会变化（now 参与），
     * 但同一时刻不同 key 不会撞在一起。 */
    const size_t span = static_cast<size_t>(delay / 5 + 1);
    size_t h = static_cast<size_t>(now_ms);
    for (char c : key) h = h * 131 + static_cast<unsigned char>(c);
    const int64_t jitter = static_cast<int64_t>(h % span);
    return delay - static_cast<int64_t>(span / 2) + jitter;
}

} /* namespace cpp_streamer */
