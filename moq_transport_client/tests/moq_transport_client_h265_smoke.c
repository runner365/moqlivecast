/* H.265 path smoke test: publish/subscribe through moqlivecast.
 * The payload is synthetic; this verifies codec signalling, catalog,
 * LOC config/payload propagation, not HEVC decoding. */
#include "moq_transport_client.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uv.h>

typedef struct {
    uv_loop_t loop;
    uv_timer_t send_timer;
    uv_timer_t sub_timer;
    uv_timer_t timeout;
    uv_timer_t finish;
    moqt_client_t *pub, *sub;
    moqt_stream_t *video, *audio;
    int vready, aready, started, sent_v, sent_a, recv_v, recv_a, done;
    const char *host, *app, *stream;
    int port;
} app_t;

/* Minimal valid hvcC: Main profile, level 90, VPS/SPS/PPS arrays. */
static const uint8_t HVCC[] = {
    0x01, 0x01, 0x60, 0x00, 0x00, 0x00, 0x90, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x5a, 0xf0, 0x00, 0xfc,
    0xfd, 0xf8, 0xf8, 0x00, 0x00, 0x0f, 0x03,
    0xa0, 0x00, 0x01, 0x00, 0x01, 0x40,
    0xa1, 0x00, 0x01, 0x00, 0x01, 0x42,
    0xa2, 0x00, 0x01, 0x00, 0x01, 0x44,
};
static const uint8_t ACFG[] = {0x12, 0x10};
static const uint8_t VFRAME[] = {0x00, 0x00, 0x00, 0x02, 0x26, 0x01};
static const uint8_t AFRAME[] = {0x21, 0x00, 0x01, 0x02};

static void finish_cb(uv_timer_t *t) { uv_stop((uv_loop_t *)t->data); }
static void on_closed(moqt_client_t *c, moqt_result_t st, const char *r, void *u) {
    (void)c; (void)st; (void)r; (void)u;
}
static void on_pub_connected(moqt_client_t *c, moqt_result_t st, const char *r, void *u) {
    (void)c; (void)u;
    printf("[pub] connected status=%d reason=%s\n", (int)st, r ? r : "");
}
static void on_sub_connected(moqt_client_t *c, moqt_result_t st, const char *r, void *u);
static void start_send(uv_timer_t *t);
static void sub_start(uv_timer_t *t);
static void on_pub_ready(moqt_client_t *c, moqt_stream_t *s,
                         const moqt_stream_info_t *i, void *u) {
    (void)c; (void)s;
    app_t *a = (app_t *)u;
    printf("[pub] ready %s codec=%d cfg=%zu\n", i->track_name,
           (int)i->codec, i->config_len);
    if (i->codec == MOQT_CODEC_H265) a->vready = 1;
    if (i->codec == MOQT_CODEC_AAC) a->aready = 1;
    if (a->vready && a->aready && !a->started) {
        a->started = 1;
        moqt_stream_write(a->video, VFRAME, sizeof(VFRAME), 0, 20, 1);
        moqt_stream_write(a->audio, AFRAME, sizeof(AFRAME), 0, 0, 0);
        a->sent_v = a->sent_a = 1;
        uv_timer_start(&a->sub_timer, sub_start, 300, 0);
    }
}
static void on_sub_ready(moqt_client_t *c, moqt_stream_t *s,
                         const moqt_stream_info_t *i, void *u) {
    (void)c; (void)s;
    printf("[sub] ready %s codec=%d cfg=%zu\n", i->track_name,
           (int)i->codec, i->config_len);
    (void)u;
}
static void on_data(moqt_client_t *c, moqt_stream_t *s, const uint8_t *d,
                    size_t n, int64_t dts, int64_t pts, int key, void *u) {
    (void)c; (void)d; (void)dts; (void)pts; (void)key;
    app_t *a = (app_t *)u;
    const char *t = moqt_stream_track_name(s);
    if (strcmp(t, "video") == 0) a->recv_v++;
    else if (strcmp(t, "audio") == 0) a->recv_a++;
    if (a->recv_v + a->recv_a <= 4)
        printf("[sub] %s len=%zu\n", t, n);
    if (a->recv_v >= 5 && a->recv_a >= 5 && !a->done) {
        a->done = 1;
        printf("PASS recv video=%d audio=%d\n", a->recv_v, a->recv_a);
        moqt_client_close(a->pub);
        moqt_client_close(a->sub);
        uv_timer_start(&a->finish, finish_cb, 100, 0);
    }
}
static void on_sub_connected(moqt_client_t *c, moqt_result_t st, const char *r, void *u) {
    (void)c;
    app_t *a = (app_t *)u;
    printf("[sub] connected status=%d reason=%s\n", (int)st, r ? r : "");
    if (st == MOQT_OK) uv_timer_start(&a->send_timer, start_send, 400, 0);
}
static void send_tick(uv_timer_t *t) {
    app_t *a = (app_t *)t->data;
    if (a->sent_v >= 10 && a->sent_a >= 10) { uv_timer_stop(t); return; }
    if (a->sent_v < 10) {
        int64_t d = 1000 + a->sent_v * 40;
        moqt_stream_write(a->video, VFRAME, sizeof(VFRAME), d, d + 20,
                          a->sent_v == 0);
        a->sent_v++;
    }
    if (a->sent_a < 10) {
        int64_t d = 1000 + a->sent_a * 40;
        moqt_stream_write(a->audio, AFRAME, sizeof(AFRAME), d, d, 0);
        a->sent_a++;
    }
}
static void start_send(uv_timer_t *t) {
    app_t *a = (app_t *)t->data;
    uv_timer_start(&a->send_timer, send_tick, 20, 20);
}
static void sub_start(uv_timer_t *t) {
    app_t *a = (app_t *)t->data;
    int r = moqt_client_connect(a->sub, a->host, a->port, "/moq", a->app,
                                a->stream, MOQT_DIRECTION_SUBSCRIBE, 5000);
    if (r != MOQT_OK) { printf("sub connect=%d\n", r); uv_stop(&a->loop); }
}
static void timeout_cb(uv_timer_t *t) {
    app_t *a = (app_t *)t->data;
    if (a->done) return;
    printf("FAIL timeout sent v=%d a=%d recv v=%d a=%d\n",
           a->sent_v, a->sent_a, a->recv_v, a->recv_a);
    a->done = 1;
    moqt_client_close(a->pub); moqt_client_close(a->sub);
    uv_stop(&a->loop);
}
int main(int argc, char **argv) {
    app_t a; memset(&a, 0, sizeof(a));
    a.host = argc > 1 ? argv[1] : "127.0.0.1";
    a.port = argc > 2 ? atoi(argv[2]) : 4433;
    a.app = argc > 3 ? argv[3] : "live";
    a.stream = argc > 4 ? argv[4] : "h265sdk";
    uv_loop_init(&a.loop);
    moqt_callbacks_t pcb = {.on_connected=on_pub_connected,
        .on_stream_ready=on_pub_ready, .on_closed=on_closed};
    moqt_callbacks_t scb = {.on_connected=on_sub_connected,
        .on_stream_ready=on_sub_ready, .on_data=on_data, .on_closed=on_closed};
    a.pub = moqt_client_new(&a.loop, &pcb, &a);
    a.sub = moqt_client_new(&a.loop, &scb, &a);
    moqt_stream_config_t v = {.codec=MOQT_CODEC_H265, .config=HVCC,
        .config_len=sizeof(HVCC), .codec_name="hvc1.1.6.L90.B0"};
    moqt_stream_config_t au = {.codec=MOQT_CODEC_AAC, .config=ACFG,
        .config_len=sizeof(ACFG), .codec_name="mp4a.40.2"};
    a.video = moqt_client_add_stream(a.pub, &v);
    a.audio = moqt_client_add_stream(a.pub, &au);
    if (!a.pub || !a.sub || !a.video || !a.audio) { printf("init failed\n"); return 1; }
    uv_timer_init(&a.loop, &a.send_timer);
    uv_timer_init(&a.loop, &a.sub_timer);
    uv_timer_init(&a.loop, &a.timeout);
    uv_timer_init(&a.loop, &a.finish);
    a.send_timer.data = &a; a.sub_timer.data = &a; a.timeout.data = &a;
    a.finish.data = &a.loop;
    uv_timer_start(&a.timeout, timeout_cb, 15000, 0);
    if (moqt_client_connect(a.pub, a.host, a.port, "/moq", a.app, a.stream,
                            MOQT_DIRECTION_PUBLISH, 5000) != MOQT_OK) {
        printf("pub connect failed\n"); return 1;
    }
    uv_run(&a.loop, UV_RUN_DEFAULT);
    uv_loop_close(&a.loop);
    return (a.recv_v >= 5 && a.recv_a >= 5) ? 0 : 1;
}
