/* ============================================
 * flv2moq_client — 把 FLV 文件按时间戳推送到 moqlivecast 的 /moq
 *
 * 用法：
 *   ./flv2moq_client input.flv https://192.168.1.1:4433/moq?app=live&&stream=123456
 *
 * 可选项：
 *   --speed <x>   倍速（默认 1.0，例如 --speed 2 表示 2 倍速）
 *   --stats <s>   每 s 秒打印一次统计（默认 5，0 表示关闭）
 * ============================================ */

#include "flv_reader.hpp"
#include "moq_publisher.hpp"

#include <uv.h>

extern "C" {
#include "logger.h"
}

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

constexpr uint64_t kTickMs = 1;
constexpr uint64_t kStatsIntervalMs = 5000;

/* ── 全部日志落文件，不写控制台 ──
 * 底层 QUIC/WT 栈和本工具都走 logger.c，统一收敛到 moq2flv_client.log。
 * 因此这里不能用 printf/fprintf，一律用 LOG_* 宏（logger.c 有后台线程
 * 定期 fflush，进程被 kill 也不容易丢最后的日志）。 */

constexpr const char *kLogFile = "moq2flv_client.log";

void OpenLog() {
    log_set_file(kLogFile);
    log_set_console(0);      /* 关掉控制台输出 */
    /* 文件里可以留全部细节：QUIC 的 per-packet DEBUG 也照收。
     * 嫌文件大可以把这里调成 INFO。 */
    log_set_level(INFO);
}

void CloseLog() {
    log_shutdown();          /* 等后台线程写完并关文件 */
}

bool g_quit = false;
uv_timer_t g_tick;
uv_timer_t g_stats;
uv_signal_t g_sigint;

struct UrlParts {
    std::string host;
    int port = 4433;
    std::string path;   /* 含 query，如 /moq?app=live&stream=123456 */
    std::string app = "live";
    std::string stream;
};

/* 从 query 里取 k=v。服务端 webtransport_server_api.c 会跳过连续的 '&'，
 * 所以 app=live&&stream=123 这种写法要能容忍空参数。 */
std::string QueryParam(const std::string &query, const std::string &key) {
    size_t pos = 0;
    while (pos < query.size()) {
        while (pos < query.size() && query[pos] == '&') pos++;
        size_t end = query.find('&', pos);
        if (end == std::string::npos) end = query.size();
        std::string kv = query.substr(pos, end - pos);
        size_t eq = kv.find('=');
        if (eq != std::string::npos && kv.substr(0, eq) == key) {
            return kv.substr(eq + 1);
        }
        pos = end + 1;
    }
    return "";
}

bool ParseUrl(const std::string &url, UrlParts &out, std::string &err) {
    std::string s = url;
    const std::string https = "https://";
    if (s.compare(0, https.size(), https) == 0) s = s.substr(https.size());
    else if (s.compare(0, 7, "http://") == 0) s = s.substr(7);

    size_t slash = s.find('/');
    std::string hostport = (slash == std::string::npos) ? s : s.substr(0, slash);
    std::string rest = (slash == std::string::npos) ? "/" : s.substr(slash);

    size_t colon = hostport.rfind(':');
    if (colon != std::string::npos) {
        out.host = hostport.substr(0, colon);
        out.port = atoi(hostport.substr(colon + 1).c_str());
    } else {
        out.host = hostport;
        out.port = 4433;
    }
    if (out.host.empty()) {
        err = "URL 缺少主机名";
        return false;
    }

    /* QuicConnectionConnect 用 uv_ip4_addr 解析，不做 DNS。
     * 传域名会在建连时才失败，这里提前拦下来给出清晰提示。 */
    struct sockaddr_in sa;
    if (uv_ip4_addr(out.host.c_str(), out.port, &sa) != 0) {
        err = "主机必须是字面 IPv4 地址（本 QUIC 栈不做 DNS 解析）: " + out.host;
        return false;
    }

    size_t q = rest.find('?');
    if (q == std::string::npos) {
        out.path = rest;
    } else {
        out.path = rest;
        std::string query = rest.substr(q + 1);
        std::string app = QueryParam(query, "app");
        std::string stream = QueryParam(query, "stream");
        if (!app.empty()) out.app = app;
        if (!stream.empty()) out.stream = stream;
    }
    if (out.stream.empty()) {
        err = "URL 缺少 stream 参数（服务端 PUBLISH 要求 app 和 stream 都非空）";
        return false;
    }
    return true;
}

void OnSigint(uv_signal_t * /*h*/, int /*signum*/) {
    LOG_INFO("[flv2moq] 收到中断，正在退出…");
    g_quit = true;
}

void OnStats(uv_timer_t *h) {
    auto *pub = static_cast<flv2moq::MoqPublisher *>(h->data);
    if (!pub) return;
    const flv2moq::Stats &st = pub->stats();
    LOG_INFO("[flv2moq] 已发 视频%zu帧(%.1fMB,丢%zu) 音频%zu帧(%.1fMB,丢%zu) "
             "最大落后%lldms dts=%lld 错误=%zu",
             st.video_sent, st.video_bytes / 1048576.0, st.video_dropped,
             st.audio_sent, st.audio_bytes / 1048576.0, st.audio_dropped,
             static_cast<long long>(st.max_late_ms),
             static_cast<long long>(st.last_dts), st.write_errors);
}

void OnTickWrapper(uv_timer_t *h) {
    auto *pub = static_cast<flv2moq::MoqPublisher *>(h->data);
    if (pub) pub->Tick();
}

double FileSizeMb(const std::string &path) {
    FILE *fp = fopen(path.c_str(), "rb");
    if (!fp) return 0.0;
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fclose(fp);
    return sz > 0 ? static_cast<double>(sz) / 1048576.0 : 0.0;
}

void Usage(const char *argv0) {
    fprintf(stderr,
            "用法: %s <input.flv> <url>\n"
            "  url 形如 https://192.168.1.1:4433/moq?app=live&&stream=123456\n"
            "可选项:\n"
            "  --speed <x>   倍速 (默认 1.0)\n"
            "  --stats <s>   统计打印间隔秒数 (默认 5, 0 关闭)\n"
            "示例:\n"
            "  %s input.flv https://192.168.1.1:4433/moq?app=live&&stream=123456\n",
            argv0, argv0);
}

} /* namespace */

int main(int argc, char **argv) {
    if (argc < 3) {
        Usage(argv[0]);
        return 1;
    }

    const std::string flv_path = argv[1];
    const std::string url = argv[2];
    double speed = 1.0;
    double stats_sec = 5.0;

    for (int i = 3; i < argc; i++) {
        if (strcmp(argv[i], "--speed") == 0 && i + 1 < argc) {
            speed = atof(argv[++i]);
            if (speed <= 0.0) {
                fprintf(stderr, "[flv2moq] --speed 必须大于 0\n");
                return 1;
            }
        } else if (strcmp(argv[i], "--stats") == 0 && i + 1 < argc) {
            stats_sec = atof(argv[++i]);
        } else {
            fprintf(stderr, "[flv2moq] 未知参数: %s\n", argv[i]);
            Usage(argv[0]);
            return 1;
        }
    }

    UrlParts parts;
    std::string err;
    if (!ParseUrl(url, parts, err)) {
        fprintf(stderr, "[flv2moq] URL 解析失败: %s\n", err.c_str());
        return 1;
    }

    /* 参数都合法了，从这里开始日志一律落文件、不碰控制台 */
    OpenLog();
    LOG_INFO("[flv2moq] 启动 input=%s url=%s speed=%.2f",
             flv_path.c_str(), url.c_str(), speed);

    /* 便宜的前置校验：只看 FLV magic，避免白连一次。
     * 完整解析留到连接建立之后 —— 连接慢、读文件快，
     * 先连接能掩盖掉读盘时间，也让「连不上」的失败来得更快。 */
    {
        std::string ferr;
        if (!flv2moq::CheckFlvMagic(flv_path, ferr)) {
            LOG_ERROR("[flv2moq] %s: %s", flv_path.c_str(), ferr.c_str());
            CloseLog();
            return 1;
        }
    }

    flv2moq::FlvReader reader;
    if (reader.Open(flv_path) != 0) {
        LOG_ERROR("[flv2moq] 打开输入文件失败: %s", flv_path.c_str());
        CloseLog();
        return 1;
    }

    uv_loop_t *loop_handle = uv_default_loop();

    flv2moq::MoqPublisher pub;
    pub.SetQuitFlag(&g_quit);
    pub.SetSpeed(speed);

    /* 先建连 + 完成信令，此时还没读过一个字节的文件 */
    LOG_INFO("[flv2moq] 输入 %s (%.1f MB)", flv_path.c_str(),
             FileSizeMb(flv_path));
    if (pub.Start(loop_handle, parts.host, parts.port, parts.path, parts.app,
                  parts.stream, &reader) != 0) {
        LOG_ERROR("[flv2moq] publisher 启动失败");
        CloseLog();
        return 1;
    }

    uv_signal_init(loop_handle, &g_sigint);
    uv_signal_start(&g_sigint, OnSigint, SIGINT);

    uv_timer_init(loop_handle, &g_tick);
    g_tick.data = &pub;
    uv_timer_start(&g_tick, OnTickWrapper, kTickMs, kTickMs);

    if (stats_sec > 0.0) {
        uv_timer_init(loop_handle, &g_stats);
        g_stats.data = &pub;
        uv_timer_start(&g_stats, OnStats,
                       static_cast<uint64_t>(stats_sec * 1000),
                       static_cast<uint64_t>(stats_sec * 1000));
    }

    /* 等连上并完成信令，最多 5 秒 —— 失败的话 publisher 会置 failed_ */
    for (int i = 0; i < 500 && !pub.ready() && !pub.failed() && !g_quit; i++) {
        uv_run(loop_handle, UV_RUN_ONCE);
    }
    if (pub.failed()) {
        LOG_ERROR("[flv2moq] 连接或信令失败，退出");
        CloseLog();
        return 1;
    }
    if (!pub.ready()) {
        LOG_ERROR("[flv2moq] 等待 WebTransport 就绪超时，退出");
        CloseLog();
        return 1;
    }

    /* 此刻才真正开始消费文件 */
    LOG_INFO("[flv2moq] 连接就绪，开始按时间戳推送 (%.2f 倍速)", speed);

    while (!g_quit) {
        uv_run(loop_handle, UV_RUN_ONCE);
    }

    if (reader.neg_cts() > 0) {
        LOG_WARN("[flv2moq] %zu 个帧的 CTS 为负，已钳为 0", reader.neg_cts());
    }
    if (reader.dropped_codec() > 0) {
        LOG_WARN("[flv2moq] 丢弃了 %zu 个非 H.264/AAC 帧",
                 reader.dropped_codec());
    }

    /* 收尾汇总：字段与周期统计保持一致，方便直接对比 */
    const flv2moq::Stats &st = pub.stats();
    const size_t v_total = st.video_sent + st.video_dropped;
    const size_t a_total = st.audio_sent + st.audio_dropped;
    LOG_INFO("[flv2moq] 结束: 视频 %zu/%zu 帧 (%s%.1f%%) 音频 %zu/%zu 帧 (%s%.1f%%) "
             "最大落后 %lldms 末帧 dts=%lld 错误=%zu",
             st.video_sent, v_total,
             st.video_dropped ? "丢 " : "",
             v_total ? 100.0 * st.video_sent / v_total : 100.0,
             st.audio_sent, a_total,
             st.audio_dropped ? "丢 " : "",
             a_total ? 100.0 * st.audio_sent / a_total : 100.0,
             static_cast<long long>(st.max_late_ms),
             static_cast<long long>(st.last_dts), st.write_errors);

    /* 先把日志落盘再拆 uv —— CloseLog 会 join 后台线程 */
    CloseLog();

    uv_timer_stop(&g_tick);
    if (stats_sec > 0.0) uv_timer_stop(&g_stats);
    uv_signal_stop(&g_sigint);
    uv_close(reinterpret_cast<uv_handle_t *>(&g_tick), nullptr);
    if (stats_sec > 0.0) uv_close(reinterpret_cast<uv_handle_t *>(&g_stats), nullptr);
    uv_close(reinterpret_cast<uv_handle_t *>(&g_sigint), nullptr);
    uv_run(loop_handle, UV_RUN_NOWAIT);

    return 0;
}
