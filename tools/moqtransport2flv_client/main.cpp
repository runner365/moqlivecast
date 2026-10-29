/* ============================================================
 * moqtransport2flv_client
 *
 * Subscribe to a moqlivecast stream with moq_transport_client and write
 * H.264 + AAC to an FLV file.  FLV muxing uses
 * moqlivecast/format/flv/FlvMuxer.
 *
 * Usage:
 *   moqtransport2flv_client <host> <port> <app> <stream> <out.flv>
 *                           [--duration seconds] [--path /moq]
 * ============================================================ */

#include "moq_transport_client.h"

#include "cpp_streamer_interface.hpp"
#include "flv_mux.hpp"
#include "media_packet.hpp"

#include <uv.h>
#include <netdb.h>
#include <sys/socket.h>

extern "C" {
#include "logger.h"
}

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

class FileSink : public cpp_streamer::CppStreamerInterface {
public:
    explicit FileSink(FILE *fp) : fp_(fp) {}

    std::string StreamerName() override { return "flv-file-sink"; }
    void SetLogger(cpp_streamer::Logger *) override {}
    int AddSinker(cpp_streamer::CppStreamerInterface *) override { return 0; }
    int RemoveSinker(const std::string &) override { return 0; }

    int SourceData(cpp_streamer::Media_Packet_Ptr pkt) override {
        if (!pkt || !fp_) return -1;
        const size_t n = pkt->buffer_ptr_->DataLen();
        if (n == 0) return 0;
        if (fwrite(pkt->buffer_ptr_->Data(), 1, n, fp_) != n) {
            error_ = true;
            return -1;
        }
        bytes_ += n;
        return 0;
    }

    void StartNetwork(const std::string &, void *) override {}
    void AddOption(const std::string &, const std::string &) override {}
    void SetReporter(cpp_streamer::StreamerReport *) override {}

    bool error() const { return error_; }
    uint64_t bytes() const { return bytes_; }

private:
    FILE *fp_ = nullptr;
    bool error_ = false;
    uint64_t bytes_ = 0;
};

struct App {
    uv_loop_t loop;
    uv_timer_t duration;
    uv_timer_t finish;
    uv_signal_t sigint;
    uv_getaddrinfo_t resolve;

    FILE *fp = nullptr;
    cpp_streamer::FlvMuxer *muxer = nullptr;
    FileSink *sink = nullptr;
    moqt_client_t *client = nullptr;

    std::string path;
    std::string host;
    int port = 4433;
    std::string appname;
    std::string stream;
    std::string output;
    std::string connect_ip;

    bool failed = false;
    bool vseq = false;
    bool aseq = false;
    bool done = false;
    bool got_media = false;
    uint64_t packets = 0;
    uint64_t payload_bytes = 0;
    double duration_sec = 0;
};

void usage(const char *argv0) {
    fprintf(stderr,
            "usage: %s <host> <port> <app> <stream> <out.flv> [options]\n"
            "  host may be an IPv4 literal or a domain with an A record\n"
            "options:\n"
            "  --duration <sec>  stop after N seconds (default: until Ctrl-C)\n"
            "  --path <path>     WebTransport path, default /moq\n"
            "example:\n"
            "  %s 127.0.0.1 4433 live demo /tmp/demo.flv --duration 10\n",
            argv0, argv0);
}

void stop_loop(App *a) {
    if (a->done) return;
    a->done = true;
    uv_timer_stop(&a->duration);
    if (a->client) moqt_client_close(a->client);
    uv_timer_start(&a->finish, [](uv_timer_t *t) {
        uv_stop((uv_loop_t *)t->data);
    }, 300, 0);
    a->finish.data = &a->loop;
}

void on_connected(moqt_client_t *, moqt_result_t status,
                  const char *reason, void *user) {
    App *a = (App *)user;
    if (status != MOQT_OK) {
        fprintf(stderr, "[moqtransport2flv] connect failed: %s\n",
                reason && reason[0] ? reason : "unknown");
        a->failed = true;
        stop_loop(a);
        return;
    }
    printf("[moqtransport2flv] connected, subscribing %s/%s\n",
           a->appname.c_str(), a->stream.c_str());
}

void write_seq(App *a, moqt_stream_t *s, moqt_codec_t codec,
               const uint8_t *cfg, size_t cfg_len) {
    if (!cfg || cfg_len == 0) return;
    if (codec == MOQT_CODEC_H264 && a->vseq) return;
    if (codec == MOQT_CODEC_AAC && a->aseq) return;

    auto pkt = std::make_shared<cpp_streamer::Media_Packet>();
    pkt->av_type_ = (codec == MOQT_CODEC_H264)
                        ? cpp_streamer::MEDIA_VIDEO_TYPE
                        : cpp_streamer::MEDIA_AUDIO_TYPE;
    pkt->codec_type_ = (codec == MOQT_CODEC_H264)
                           ? cpp_streamer::MEDIA_CODEC_H264
                           : cpp_streamer::MEDIA_CODEC_AAC;
    pkt->is_seq_hdr_ = true;
    pkt->is_key_frame_ = false;
    pkt->dts_ = 0;
    pkt->pts_ = 0;
    pkt->buffer_ptr_->AppendData((const char *)cfg, cfg_len);
    if (a->muxer->InputPacket(pkt) < 0) {
        fprintf(stderr, "[moqtransport2flv] seq header mux failed\n");
        stop_loop(a);
        return;
    }
    if (codec == MOQT_CODEC_H264) a->vseq = true;
    else a->aseq = true;
    printf("[moqtransport2flv] wrote %s sequence header (%zu bytes)\n",
           codec == MOQT_CODEC_H264 ? "video" : "audio", cfg_len);
    (void)s;
}

void on_stream_ready(moqt_client_t *, moqt_stream_t *s,
                     const moqt_stream_info_t *info, void *user) {
    App *a = (App *)user;
    printf("[moqtransport2flv] stream %s ready codec=%s cfg=%zu\n",
           info->track_name,
           info->codec == MOQT_CODEC_H264 ? "H264" : "AAC",
           info->config_len);
    write_seq(a, s, info->codec, info->config, info->config_len);
}

void write_media(App *a, moqt_stream_t *s,
                 const uint8_t *data, size_t len,
                 int64_t dts_ms, int64_t pts_ms, int keyframe) {
    moqt_codec_t codec = moqt_stream_codec(s);
    if (codec == MOQT_CODEC_H264 && !a->vseq) {
        size_t cfg_len = 0;
        const uint8_t *cfg = moqt_stream_config(s, &cfg_len);
        write_seq(a, s, codec, cfg, cfg_len);
    } else if (codec == MOQT_CODEC_AAC && !a->aseq) {
        size_t cfg_len = 0;
        const uint8_t *cfg = moqt_stream_config(s, &cfg_len);
        write_seq(a, s, codec, cfg, cfg_len);
    }

    auto pkt = std::make_shared<cpp_streamer::Media_Packet>();
    pkt->av_type_ = (codec == MOQT_CODEC_H264)
                        ? cpp_streamer::MEDIA_VIDEO_TYPE
                        : cpp_streamer::MEDIA_AUDIO_TYPE;
    pkt->codec_type_ = (codec == MOQT_CODEC_H264)
                           ? cpp_streamer::MEDIA_CODEC_H264
                           : cpp_streamer::MEDIA_CODEC_AAC;
    pkt->is_seq_hdr_ = false;
    pkt->is_key_frame_ = keyframe != 0;
    pkt->dts_ = dts_ms;
    pkt->pts_ = pts_ms;
    pkt->buffer_ptr_->AppendData((const char *)data, len);

    if (a->muxer->InputPacket(pkt) < 0) {
        fprintf(stderr, "[moqtransport2flv] media mux failed\n");
        stop_loop(a);
        return;
    }
    a->packets++;
    a->payload_bytes += len;
    a->got_media = true;
}

void on_data(moqt_client_t *, moqt_stream_t *s,
             const uint8_t *data, size_t len,
             int64_t dts_ms, int64_t pts_ms, int keyframe, void *user) {
    App *a = (App *)user;
    if (!data || len == 0) return;

    size_t cfg_len = 0;
    const uint8_t *cfg = moqt_stream_config(s, &cfg_len);
    if (cfg && cfg_len == len && memcmp(cfg, data, len) == 0) {
        /* The first object can be the config-only object; the seq header
         * has already been written in on_stream_ready. */
        moqt_codec_t codec = moqt_stream_codec(s);
        write_seq(a, s, codec, cfg, cfg_len);
        return;
    }
    write_media(a, s, data, len, dts_ms, pts_ms, keyframe);
}

void on_closed(moqt_client_t *, moqt_result_t status,
               const char *reason, void *user) {
    App *a = (App *)user;
    printf("[moqtransport2flv] closed status=%d reason=%s\n",
           (int)status, reason ? reason : "");
    if (!a->done) {
        a->done = true;
        uv_stop(&a->loop);
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
        fprintf(stderr, "[moqtransport2flv] DNS %s failed: %s\n",
                a->host.c_str(),
                status != 0 ? uv_strerror(status) : "no IPv4 A record");
        a->failed = true;
        uv_stop(&a->loop);
        return;
    }

    a->connect_ip = ip;
    printf("[moqtransport2flv] resolved %s -> %s\n",
           a->host.c_str(), a->connect_ip.c_str());
    moqt_result_t r = moqt_client_connect_ex(
        a->client, a->host.c_str(), a->connect_ip.c_str(), a->port,
        a->path.c_str(), a->appname.c_str(), a->stream.c_str(),
        MOQT_DIRECTION_SUBSCRIBE, 5000);
    if (r != MOQT_OK) {
        fprintf(stderr, "[moqtransport2flv] connect_ex failed: %d\n", (int)r);
        a->failed = true;
        uv_stop(&a->loop);
    }
}

void on_duration(uv_timer_t *t) {
    App *a = (App *)t->data;
    printf("[moqtransport2flv] duration reached\n");
    stop_loop(a);
}

void on_sigint(uv_signal_t *h, int signum) {
    (void)signum;
    App *a = (App *)h->data;
    printf("[moqtransport2flv] signal, stopping\n");
    stop_loop(a);
}

} /* namespace */

int main(int argc, char **argv) {
    if (argc < 6) {
        usage(argv[0]);
        return 1;
    }

    App a;
    a.host = argv[1];
    a.port = atoi(argv[2]);
    a.appname = argv[3];
    a.stream = argv[4];
    a.output = argv[5];
    a.path = "/moq";

    for (int i = 6; i < argc; i++) {
        if (strcmp(argv[i], "--duration") == 0 && i + 1 < argc) {
            a.duration_sec = atof(argv[++i]);
        } else if (strcmp(argv[i], "--path") == 0 && i + 1 < argc) {
            a.path = argv[++i];
        } else {
            fprintf(stderr, "unknown option: %s\n", argv[i]);
            usage(argv[0]);
            return 1;
        }
    }

    if (a.port <= 0 || a.port > 65535) {
        fprintf(stderr, "invalid port: %d\n", a.port);
        return 1;
    }

    a.fp = fopen(a.output.c_str(), "wb");
    if (!a.fp) {
        fprintf(stderr, "cannot open output: %s\n", a.output.c_str());
        return 1;
    }

    log_set_console(1);
    log_set_level(WARN);

    uv_loop_init(&a.loop);
    a.muxer = new cpp_streamer::FlvMuxer();
    a.muxer->SetLogger(nullptr);
    a.sink = new FileSink(a.fp);
    a.muxer->AddSinker(a.sink);

    moqt_callbacks_t cbs;
    memset(&cbs, 0, sizeof(cbs));
    cbs.on_connected = on_connected;
    cbs.on_stream_ready = on_stream_ready;
    cbs.on_data = on_data;
    cbs.on_closed = on_closed;
    a.client = moqt_client_new(&a.loop, &cbs, &a);
    if (!a.client) {
        fprintf(stderr, "moqt_client_new failed\n");
        return 1;
    }

    uv_timer_init(&a.loop, &a.duration);
    uv_timer_init(&a.loop, &a.finish);
    uv_signal_init(&a.loop, &a.sigint);
    a.duration.data = &a;
    a.finish.data = &a.loop;
    a.sigint.data = &a;
    uv_signal_start(&a.sigint, on_sigint, SIGINT);
    if (a.duration_sec > 0) {
        uv_timer_start(&a.duration, on_duration,
                       (uint64_t)(a.duration_sec * 1000), 0);
    }

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

    if (a.client) moqt_client_free(a.client);
    delete a.muxer;
    delete a.sink;
    fflush(a.fp);
    fclose(a.fp);
    uv_loop_close(&a.loop);
    log_shutdown();

    printf("[moqtransport2flv] wrote %llu packets, %llu payload bytes to %s\n",
           (unsigned long long)a.packets,
           (unsigned long long)a.payload_bytes, a.output.c_str());
    return (a.got_media && !a.failed) ? 0 : 1;
}
