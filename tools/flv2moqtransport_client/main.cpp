/* ============================================================
 * flv2moqtransport_client
 *
 * Read an FLV file, extract H.264 (AVCC) + AAC frames with DTS/PTS,
 * and publish them through moq_transport_client at the file's DTS pace.
 *
 * Usage:
 *   flv2moqtransport_client <input.flv> <host> <port> <app> <stream>
 *                           [--speed x] [--stats seconds] [--path /moq]
 * ============================================================ */

#include "flv_reader.hpp"
#include "moq_transport_client.h"

#include <uv.h>
#include <netdb.h>
#include <sys/socket.h>

extern "C" {
#include "logger.h"
}

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <string>
#include <utility>
#include <vector>

namespace {

struct App {
    uv_loop_t loop;
    uv_timer_t tick;
    uv_timer_t stats;
    uv_timer_t finish;
    uv_getaddrinfo_t resolve;

    flv2moq::FlvReader reader;
    std::deque<flv2moq::FlvFrame> pending;

    moqt_client_t *client = nullptr;
    moqt_stream_t *video = nullptr;
    moqt_stream_t *audio = nullptr;

    std::string host;
    int port = 4433;
    std::string path;
    std::string appname;
    std::string stream;
    std::string connect_ip;

    std::vector<uint8_t> vcfg;
    std::vector<uint8_t> acfg;

    bool video_ready = false;
    bool audio_ready = false;
    bool started = false;
    bool eof = false;
    bool finishing = false;
    bool done = false;

    int64_t first_dts = -1;
    uint64_t start_wall_ms = 0;
    double speed = 1.0;

    uint64_t sent_video = 0;
    uint64_t sent_audio = 0;
    uint64_t acked = 0;
    uint64_t write_errors = 0;
    uint64_t bytes = 0;
    bool failed = false;
};

void usage(const char *argv0) {
    fprintf(stderr,
            "usage: %s <input.flv> <host> <port> <app> <stream> [options]\n"
            "  host may be an IPv4 literal or a domain with an A record\n"
            "options:\n"
            "  --speed <x>       playback speed, default 1.0\n"
            "  --stats <sec>     stats interval, default 5, 0 disables\n"
            "  --path <path>     WebTransport path, default /moq\n"
            "example:\n"
            "  %s test.flv 127.0.0.1 4433 live demo\n",
            argv0, argv0);
}

void stop_loop(App *a) {
    if (a->done) return;
    a->done = true;
    uv_timer_stop(&a->tick);
    uv_timer_stop(&a->stats);
    if (a->client) moqt_client_close(a->client);
    uv_timer_start(&a->finish, [](uv_timer_t *t) {
        uv_stop((uv_loop_t *)t->data);
    }, 100, 0);
    a->finish.data = &a->loop;
}

void on_connected(moqt_client_t *, moqt_result_t status,
                  const char *reason, void *user) {
    App *a = (App *)user;
    if (status != MOQT_OK) {
        fprintf(stderr, "[flv2moqtransport] connect failed: %s\n",
                reason && reason[0] ? reason : "unknown");
        a->failed = true;
        stop_loop(a);
        return;
    }
    printf("[flv2moqtransport] connected, waiting for publish streams\n");
}

void on_stream_ready(moqt_client_t *, moqt_stream_t *s,
                     const moqt_stream_info_t *info, void *user) {
    App *a = (App *)user;
    if (info->codec == MOQT_CODEC_H264) a->video_ready = true;
    if (info->codec == MOQT_CODEC_AAC) a->audio_ready = true;
    printf("[flv2moqtransport] stream %s ready codec=%s cfg=%zu\n",
           info->track_name,
           info->codec == MOQT_CODEC_H264 ? "H264" : "AAC",
           info->config_len);
    if (a->video_ready && a->audio_ready && !a->started) {
        a->started = true;
        a->start_wall_ms = uv_now(&a->loop);
        if (a->first_dts < 0 && !a->pending.empty())
            a->first_dts = a->pending.front().dts;
        printf("[flv2moqtransport] publishing at %.2fx\n", a->speed);
    }
    (void)s;
}

void on_write_done(moqt_stream_t *s, moqt_result_t status,
                   size_t nbytes, void *user) {
    App *a = (App *)user;
    if (status == MOQT_OK) {
        a->acked++;
        a->bytes += nbytes;
    } else {
        a->write_errors++;
        fprintf(stderr, "[flv2moqtransport] write %s failed: status=%d bytes=%zu\n",
                moqt_stream_track_name(s), (int)status, nbytes);
    }
    (void)s;
}

void on_closed(moqt_client_t *, moqt_result_t status,
               const char *reason, void *user) {
    App *a = (App *)user;
    printf("[flv2moqtransport] closed status=%d reason=%s\n",
           (int)status, reason ? reason : "");
    if (!a->done) {
        a->done = true;
        uv_stop(&a->loop);
    }
}

void fill_pending(App *a) {
    while (!a->reader.Empty()) {
        flv2moq::FlvFrame f = a->reader.TakeFront();
        if (f.is_seq_hdr) {
            if (f.is_video) a->vcfg = std::move(f.payload);
            else a->acfg = std::move(f.payload);
            continue;
        }
        a->pending.push_back(std::move(f));
    }
    if (a->reader.Eof()) {
        a->eof = true;
        return;
    }
    if (!a->reader.Fill()) {
        a->eof = true;
        return;
    }
    while (!a->reader.Empty()) {
        flv2moq::FlvFrame f = a->reader.TakeFront();
        if (f.is_seq_hdr) {
            if (f.is_video) a->vcfg = std::move(f.payload);
            else a->acfg = std::move(f.payload);
            continue;
        }
        a->pending.push_back(std::move(f));
    }
    if (a->reader.Eof()) a->eof = true;
}

void on_tick(uv_timer_t *t) {
    App *a = (App *)t->data;
    if (!a->started || a->done) return;

    if (a->pending.empty() && !a->eof) fill_pending(a);
    if (a->first_dts < 0 && !a->pending.empty())
        a->first_dts = a->pending.front().dts;
    if (a->first_dts < 0) {
        if (a->eof && a->pending.empty() && !a->finishing) {
            a->finishing = true;
            printf("[flv2moqtransport] input has no media frames\n");
            uv_timer_start(&a->finish, [](uv_timer_t *ft) {
                uv_stop((uv_loop_t *)ft->data);
            }, 200, 0);
            a->finish.data = &a->loop;
        }
        return;
    }

    const uint64_t now = uv_now(&a->loop);
    while (!a->pending.empty()) {
        flv2moq::FlvFrame &f = a->pending.front();
        if (f.is_seq_hdr) { a->pending.pop_front(); continue; }
        const double offset = (double)(f.dts - a->first_dts) / a->speed;
        if (offset < 0 || now < a->start_wall_ms + (uint64_t)offset) break;

        moqt_stream_t *s = f.is_video ? a->video : a->audio;
        moqt_result_t r = moqt_stream_write_ex(
            s, f.payload.data(), f.payload.size(),
            f.dts, f.dts + f.cts, f.key,
            on_write_done, a);
        if (r == MOQT_ERR_AGAIN) break;  /* caller/sdk high watermark */
        if (r != MOQT_OK) {
            fprintf(stderr, "[flv2moqtransport] write queue failed: %d\n", (int)r);
            a->write_errors++;
            a->failed = true;
        }
        if (f.is_video) a->sent_video++;
        else a->sent_audio++;
        a->pending.pop_front();
    }

    if (a->pending.empty() && !a->eof) {
        fill_pending(a);
    }
    if (a->eof && a->pending.empty() && !a->finishing) {
        a->finishing = true;
        printf("[flv2moqtransport] input done: video=%llu audio=%llu "
               "acked=%llu bytes=%llu errors=%llu\n",
               (unsigned long long)a->sent_video,
               (unsigned long long)a->sent_audio,
               (unsigned long long)a->acked,
               (unsigned long long)a->bytes,
               (unsigned long long)a->write_errors);
        uv_timer_start(&a->finish, [](uv_timer_t *ft) {
            uv_stop((uv_loop_t *)ft->data);
        }, 1000, 0);
        a->finish.data = &a->loop;
    }
}

void on_resolved(uv_getaddrinfo_t *req, int status, struct addrinfo *res) {
    App *a = (App *)req->data;
    if (a->done) {
        if (res) uv_freeaddrinfo(res);
        return;
    }
    char ip[64] = {0};
    if (status == 0 && res) {
        for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
            if (ai->ai_family == AF_INET && ai->ai_addr) {
                uv_ip4_name((const struct sockaddr_in *)ai->ai_addr,
                            ip, sizeof(ip));
                break;
            }
        }
    }
    if (res) uv_freeaddrinfo(res);
    if (status != 0 || ip[0] == 0) {
        fprintf(stderr, "[flv2moqtransport] DNS %s failed: %s\n",
                a->host.c_str(),
                status != 0 ? uv_strerror(status) : "no IPv4 A record");
        a->failed = true;
        uv_stop(&a->loop);
        return;
    }

    a->connect_ip = ip;
    printf("[flv2moqtransport] resolved %s -> %s\n",
           a->host.c_str(), a->connect_ip.c_str());
    moqt_result_t r = moqt_client_connect_ex(
        a->client, a->host.c_str(), a->connect_ip.c_str(), a->port,
        a->path.c_str(), a->appname.c_str(), a->stream.c_str(),
        MOQT_DIRECTION_PUBLISH, 5000);
    if (r != MOQT_OK) {
        fprintf(stderr, "[flv2moqtransport] connect_ex failed: %d\n", (int)r);
        a->failed = true;
        uv_stop(&a->loop);
    }
}

void on_stats(uv_timer_t *t) {
    App *a = (App *)t->data;
    if (a->done) return;
    printf("[flv2moqtransport] sent v=%llu a=%llu acked=%llu bytes=%llu "
           "errors=%llu pending=%zu eof=%d\n",
           (unsigned long long)a->sent_video,
           (unsigned long long)a->sent_audio,
           (unsigned long long)a->acked,
           (unsigned long long)a->bytes,
           (unsigned long long)a->write_errors,
           a->pending.size(), a->eof ? 1 : 0);
}

} /* namespace */

int main(int argc, char **argv) {
    if (argc < 6) {
        usage(argv[0]);
        return 1;
    }

    const std::string path_arg = argv[1];
    const std::string host = argv[2];
    const int port = atoi(argv[3]);
    const std::string appname = argv[4];
    const std::string stream = argv[5];
    std::string wt_path = "/moq";
    double speed = 1.0;
    double stats_sec = 5.0;

    for (int i = 6; i < argc; i++) {
        if (strcmp(argv[i], "--speed") == 0 && i + 1 < argc) {
            speed = atof(argv[++i]);
        } else if (strcmp(argv[i], "--stats") == 0 && i + 1 < argc) {
            stats_sec = atof(argv[++i]);
        } else if (strcmp(argv[i], "--path") == 0 && i + 1 < argc) {
            wt_path = argv[++i];
        } else {
            fprintf(stderr, "unknown option: %s\n", argv[i]);
            usage(argv[0]);
            return 1;
        }
    }
    if (speed <= 0.0) {
        fprintf(stderr, "--speed must be > 0\n");
        return 1;
    }

    if (port <= 0 || port > 65535) {
        fprintf(stderr, "invalid port: %d\n", port);
        return 1;
    }

    log_set_console(1);
    log_set_level(WARN);

    App a;
    a.speed = speed;
    a.host = host;
    a.port = port;
    a.path = wt_path;
    a.appname = appname;
    a.stream = stream;
    uv_loop_init(&a.loop);

    std::string ferr;
    if (!flv2moq::CheckFlvMagic(path_arg, ferr)) {
        fprintf(stderr, "FLV check failed: %s\n", ferr.c_str());
        return 1;
    }
    if (a.reader.Open(path_arg) != 0) {
        fprintf(stderr, "cannot open %s\n", path_arg.c_str());
        return 1;
    }

    /* Read until both sequence headers are found. */
    while ((a.vcfg.empty() || a.acfg.empty()) && a.reader.Fill()) {
        while (!a.reader.Empty()) {
            flv2moq::FlvFrame f = a.reader.TakeFront();
            if (f.is_seq_hdr) {
                if (f.is_video) a.vcfg = std::move(f.payload);
                else a.acfg = std::move(f.payload);
            } else {
                a.pending.push_back(std::move(f));
            }
        }
    }
    if (a.vcfg.empty() || a.acfg.empty()) {
        fprintf(stderr, "FLV missing H.264 avcC or AAC AudioSpecificConfig "
                        "(video=%zu audio=%zu)\n",
                a.vcfg.size(), a.acfg.size());
        return 1;
    }
    if (!a.pending.empty()) a.first_dts = a.pending.front().dts;

    moqt_callbacks_t cbs;
    memset(&cbs, 0, sizeof(cbs));
    cbs.on_connected = on_connected;
    cbs.on_stream_ready = on_stream_ready;
    cbs.on_closed = on_closed;
    a.client = moqt_client_new(&a.loop, &cbs, &a);
    if (!a.client) {
        fprintf(stderr, "moqt_client_new failed\n");
        return 1;
    }

    moqt_stream_config_t v = {
        .codec = MOQT_CODEC_H264,
        .config = a.vcfg.data(),
        .config_len = a.vcfg.size(),
        .codec_name = "avc1.64001f",
    };
    moqt_stream_config_t au = {
        .codec = MOQT_CODEC_AAC,
        .config = a.acfg.data(),
        .config_len = a.acfg.size(),
        .codec_name = "mp4a.40.2",
    };
    a.video = moqt_client_add_stream(a.client, &v);
    a.audio = moqt_client_add_stream(a.client, &au);
    if (!a.video || !a.audio) {
        fprintf(stderr, "add_stream failed\n");
        return 1;
    }

    uv_timer_init(&a.loop, &a.tick);
    uv_timer_init(&a.loop, &a.stats);
    uv_timer_init(&a.loop, &a.finish);
    a.tick.data = &a;
    a.stats.data = &a;
    a.finish.data = &a.loop;
    uv_timer_start(&a.tick, on_tick, 10, 10);
    if (stats_sec > 0)
        uv_timer_start(&a.stats, on_stats, (uint64_t)(stats_sec * 1000),
                       (uint64_t)(stats_sec * 1000));

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;
    char portstr[16];
    snprintf(portstr, sizeof(portstr), "%d", a.port);
    a.resolve.data = &a;
    int rc = uv_getaddrinfo(&a.loop, &a.resolve, on_resolved,
                            a.host.c_str(), portstr, &hints);
    if (rc != 0) {
        fprintf(stderr, "uv_getaddrinfo(%s) failed: %s\n",
                a.host.c_str(), uv_strerror(rc));
        return 1;
    }

    uv_run(&a.loop, UV_RUN_DEFAULT);
    uv_loop_close(&a.loop);
    log_shutdown();
    return (a.write_errors == 0 && !a.failed) ? 0 : 1;
}
