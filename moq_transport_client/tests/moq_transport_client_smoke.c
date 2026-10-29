/* Publish + subscribe smoke test for moq_transport_client.
 *
 * Run moqlivecast first (path /moq), then:
 *   ./build/bin/moq_transport_client_smoke 127.0.0.1 4433 live sdk_test
 */

#include "moq_transport_client.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uv.h>

typedef struct {
    uv_loop_t loop;
    uv_timer_t send_timer;
    uv_timer_t sub_timer;
    uv_timer_t timeout_timer;
    uv_timer_t exit_timer;

    moqt_client_t *pub;
    moqt_client_t *sub;
    moqt_stream_t *pub_video;
    moqt_stream_t *pub_audio;

    int pub_video_ready;
    int pub_audio_ready;
    int sent_video;
    int sent_audio;
    int sub_started;
    const char *host;
    int port;
    const char *appname;
    const char *stream;
    int recv_video;
    int recv_audio;
    int write_ok;
    int write_err;
    int wm_again;
    int done;
} app_t;

static const uint8_t VCFG[] = {
    0x01, 0x64, 0x00, 0x1f, 0xff, 0xe1, 0x00, 0x04,
    0x67, 0x64, 0x00, 0x1f, 0x01, 0x00, 0x04, 0x68,
    0xee, 0x3c, 0x80
};
static const uint8_t ACFG[] = { 0x12, 0x10 };
static const uint8_t VFRAME[] = { 0x00, 0x00, 0x00, 0x02, 0x09, 0xf0 };
static const uint8_t AFRAME[] = { 0x21, 0x00, 0x01, 0x02 };

static void on_write_done(moqt_stream_t *s, moqt_result_t status,
                          size_t bytes, void *user) {
    app_t *a = (app_t *)user;
    (void)s;
    if (status == MOQT_OK && bytes > 0) a->write_ok++;
    else {
        a->write_err++;
        printf("[write] status=%d bytes=%zu\n", (int)status, bytes);
    }
}

static void on_closed(moqt_client_t *c, moqt_result_t status,
                      const char *reason, void *user) {
    (void)c;
    (void)user;
    printf("[closed] status=%d reason=%s\n", (int)status,
           reason ? reason : "");
}

static void on_pub_connected(moqt_client_t *c, moqt_result_t status,
                             const char *reason, void *user) {
    (void)c;
    (void)user;
    printf("[pub] connected status=%d reason=%s\n", (int)status,
           reason ? reason : "");
}

static void start_send_cb(uv_timer_t *t);
static void sub_start_cb(uv_timer_t *t);

static void on_pub_stream_ready(moqt_client_t *c, moqt_stream_t *s,
                                const moqt_stream_info_t *info, void *user) {
    (void)c;
    app_t *a = (app_t *)user;
    printf("[pub] stream %s codec=%d cfg=%zu\n", info->track_name,
           (int)info->codec, info->config_len);
    if (s == a->pub_video) a->pub_video_ready = 1;
    if (s == a->pub_audio) a->pub_audio_ready = 1;
    if (a->pub_video_ready && a->pub_audio_ready && !a->sub_started) {
        /* Seed the GOP before the subscriber arrives: this mirrors a real
         * publisher that is already live when a player connects. */
        a->sub_started = 1;
        moqt_stream_set_high_watermark(a->pub_video, 8);
        moqt_stream_write_ex(a->pub_video, VFRAME, sizeof(VFRAME), 0, 20, 1,
                             on_write_done, a);
        if (moqt_stream_write(a->pub_video, VFRAME, sizeof(VFRAME), 0, 0, 0)
            == MOQT_ERR_AGAIN)
            a->wm_again = 1;
        moqt_stream_set_high_watermark(a->pub_video, 0);
        moqt_stream_write_ex(a->pub_audio, AFRAME, sizeof(AFRAME), 0, 0, 0,
                             on_write_done, a);
        a->sent_video = 1;
        a->sent_audio = 1;
        uv_timer_start(&a->sub_timer, sub_start_cb, 300, 0);
    }
}

static void on_sub_connected(moqt_client_t *c, moqt_result_t status,
                             const char *reason, void *user) {
    (void)c;
    app_t *a = (app_t *)user;
    printf("[sub] connected status=%d reason=%s\n", (int)status,
           reason ? reason : "");
    if (status == MOQT_OK)
        uv_timer_start(&a->send_timer, start_send_cb, 500, 0);
}

static void on_sub_stream_ready(moqt_client_t *c, moqt_stream_t *s,
                                const moqt_stream_info_t *info, void *user) {
    (void)c;
    (void)user;
    printf("[sub] stream %s codec=%d cfg=%zu\n", info->track_name,
           (int)info->codec, info->config_len);
    (void)s;
}

static void exit_cb(uv_timer_t *t);

static void maybe_done(app_t *a) {
    if (a->done) return;
    if (a->recv_video >= 10 && a->recv_audio >= 10) {
        a->done = 1;
        printf("PASS recv video=%d audio=%d write_ok=%d write_err=%d "
               "wm_again=%d inflight_v=%zu inflight_a=%zu\n",
               a->recv_video, a->recv_audio, a->write_ok, a->write_err,
               a->wm_again,
               moqt_stream_inflight_bytes(a->pub_video),
               moqt_stream_inflight_bytes(a->pub_audio));
        uv_timer_start(&a->exit_timer, exit_cb, 800, 0);
        moqt_client_close(a->pub);
        moqt_client_close(a->sub);
    }
}

static void on_sub_data(moqt_client_t *c, moqt_stream_t *s,
                        const uint8_t *data, size_t len,
                        int64_t dts_ms, int64_t pts_ms, int keyframe,
                        void *user) {
    (void)c;
    app_t *a = (app_t *)user;
    const char *track = moqt_stream_track_name(s);
    if (strcmp(track, "video") == 0) a->recv_video++;
    else if (strcmp(track, "audio") == 0) a->recv_audio++;
    if (a->recv_video + a->recv_audio <= 4) {
        printf("[sub] %s len=%zu dts=%lld pts=%lld key=%d\n",
               track, len, (long long)dts_ms, (long long)pts_ms, keyframe);
    }
    (void)data;
    maybe_done(a);
}

static void send_tick(uv_timer_t *t) {
    app_t *a = (app_t *)t->data;
    if (a->sent_video >= 10 && a->sent_audio >= 10) {
        uv_timer_stop(t);
        return;
    }
    if (a->sent_video < 10) {
        int64_t dts = 1000 + a->sent_video * 40;
        moqt_stream_write_ex(a->pub_video, VFRAME, sizeof(VFRAME),
                             dts, dts + 20, a->sent_video == 0,
                             on_write_done, a);
        a->sent_video++;
    }
    if (a->sent_audio < 10) {
        int64_t dts = 1000 + a->sent_audio * 40;
        moqt_stream_write_ex(a->pub_audio, AFRAME, sizeof(AFRAME),
                             dts, dts, 0, on_write_done, a);
        a->sent_audio++;
    }
}

static void start_send_cb(uv_timer_t *t) {
    app_t *a = (app_t *)t->data;
    uv_timer_start(&a->send_timer, send_tick, 20, 20);
}

static void sub_start_cb(uv_timer_t *t) {
    app_t *a = (app_t *)t->data;
    if (moqt_client_connect(a->sub, a->host, a->port, "/moq", a->appname,
                            a->stream, MOQT_DIRECTION_SUBSCRIBE, 5000) != MOQT_OK) {
        printf("sub connect failed\n");
        uv_stop(&a->loop);
    }
}

static void timeout_cb(uv_timer_t *t) {
    app_t *a = (app_t *)t->data;
    if (a->done) return;
    printf("FAIL timeout: sent v=%d a=%d recv v=%d a=%d\n",
           a->sent_video, a->sent_audio, a->recv_video, a->recv_audio);
    a->done = 1;
    moqt_client_close(a->pub);
    moqt_client_close(a->sub);
    uv_stop(&a->loop);
}

static void exit_cb(uv_timer_t *t) {
    app_t *a = (app_t *)t->data;
    uv_stop(&a->loop);
}

int main(int argc, char **argv) {
    const char *host = argc > 1 ? argv[1] : "127.0.0.1";
    int port = argc > 2 ? atoi(argv[2]) : 4433;
    const char *appname = argc > 3 ? argv[3] : "live";
    const char *stream = argc > 4 ? argv[4] : "sdk_test";

    app_t a;
    memset(&a, 0, sizeof(a));
    a.host = host;
    a.port = port;
    a.appname = appname;
    a.stream = stream;
    uv_loop_init(&a.loop);

    moqt_callbacks_t pubcb = {
        .on_connected = on_pub_connected,
        .on_stream_ready = on_pub_stream_ready,
        .on_closed = on_closed,
    };
    moqt_callbacks_t subcb = {
        .on_connected = on_sub_connected,
        .on_stream_ready = on_sub_stream_ready,
        .on_data = on_sub_data,
        .on_closed = on_closed,
    };

    a.pub = moqt_client_new(&a.loop, &pubcb, &a);
    a.sub = moqt_client_new(&a.loop, &subcb, &a);
    if (!a.pub || !a.sub) {
        printf("client_new failed\n");
        return 1;
    }

    moqt_stream_config_t v = {
        .codec = MOQT_CODEC_H264,
        .config = VCFG,
        .config_len = sizeof(VCFG),
        .codec_name = "avc1.64001f",
    };
    moqt_stream_config_t au = {
        .codec = MOQT_CODEC_AAC,
        .config = ACFG,
        .config_len = sizeof(ACFG),
        .codec_name = "mp4a.40.2",
    };
    a.pub_video = moqt_client_add_stream(a.pub, &v);
    a.pub_audio = moqt_client_add_stream(a.pub, &au);
    if (!a.pub_video || !a.pub_audio) {
        printf("add_stream failed\n");
        return 1;
    }

    uv_timer_init(&a.loop, &a.send_timer);
    uv_timer_init(&a.loop, &a.sub_timer);
    uv_timer_init(&a.loop, &a.timeout_timer);
    uv_timer_init(&a.loop, &a.exit_timer);
    a.send_timer.data = &a;
    a.sub_timer.data = &a;
    a.timeout_timer.data = &a;
    a.exit_timer.data = &a;
    uv_timer_start(&a.timeout_timer, timeout_cb, 15000, 0);

    if (moqt_client_connect(a.pub, host, port, "/moq", appname, stream,
                            MOQT_DIRECTION_PUBLISH, 5000) != MOQT_OK) {
        printf("pub connect failed\n");
        return 1;
    }

    uv_run(&a.loop, UV_RUN_DEFAULT);
    uv_loop_close(&a.loop);
    return (a.recv_video >= 10 && a.recv_audio >= 10) ? 0 : 1;
}
