#include "moq_transport_client.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "webtransport_client_api.h"

/* ============================================================
 * moq-transport draft-22 over WebTransport.
 *
 * Wire layout (matches moqlivecast/moq/moq_push.cpp and the JS client):
 *   control  : uni stream, SETUP
 *   requests : one bidi stream per PUBLISH/SUBSCRIBE
 *   objects  : publisher opens a uni stream per track, starts with a
 *              SUBGROUP header, then OBJECT messages
 *   subscriber receives server-opened uni streams; the SUBGROUP alias
 *   identifies the track.
 * ============================================================ */

#define MOQT_SETUP       0x2f00ULL
#define MOQT_PUBLISH     0x1dULL
#define MOQT_SUBSCRIBE   0x03ULL

#define LOC_TIMESCALE    0x08ULL
#define LOC_FRAME_MARK   0x09ULL
#define LOC_VIDEO_CONFIG 0x0dULL
#define LOC_COMPOSITION  0x0eULL
#define LOC_AUDIO_CONFIG 0x0fULL
#define LOC_TIMESTAMP    0x10ULL

#define SUBGROUP_FLAGS   0x31ULL

#define ALIAS_CATALOG 0ULL
#define ALIAS_VIDEO   1ULL
#define ALIAS_AUDIO   2ULL

#define MAX_TRACK 16
#define MAX_CODEC 32

typedef struct {
    uint8_t *p;
    size_t   len;
    size_t   cap;
} moq_buf;

struct moqt_stream {
    moqt_client_t *client;
    moqt_codec_t   codec;
    char           track[MAX_TRACK];
    char           codec_name[MAX_CODEC];
    uint8_t       *config;
    size_t         config_len;
    uint64_t       alias;
    int            ready;       /* publisher: data stream open */
    int            announced;   /* subscriber: on_stream_ready fired */
    int            config_sent; /* publisher: config attached once */
    size_t         inflight;    /* payload bytes queued, not ACKed */
    size_t         high_watermark; /* 0 = unlimited */
    wt_stream_t   *wt;
    moq_buf        rx;          /* subscriber incremental parser */
    int            header_done;
    int            has_props;
    uint64_t       group_id;
    struct moqt_stream *next;
};

enum {
    C_IDLE = 0,
    C_CONNECTING,
    C_CONNECTED,
    C_CLOSED
};

enum {
    PUB_CONTROL = 0,
    PUB_REQ_CATALOG,
    PUB_REQ_VIDEO,
    PUB_REQ_AUDIO,
    PUB_DATA_CATALOG,
    PUB_DATA_VIDEO,
    PUB_DATA_AUDIO,
    PUB_DONE
};

struct moqt_client {
    uv_loop_t       *loop;
    wt_client_t     *wt;
    moqt_callbacks_t cb;
    void            *user;

    char *host;
    int   port;
    char *path;
    char *app;
    char *stream;
    moqt_direction_t dir;

    int state;
    int pub_opened;
    int sub_step;
    wt_stream_t *control;
    wt_stream_t *req;

    moqt_stream_t *streams;
    moqt_stream_t *video;
    moqt_stream_t *audio;

    int closing;
    char close_reason[128];

    uv_timer_t connect_timer;
    int connect_timeout_ms;
    int connect_reported;
    int close_reported;
};

/* ---------------- memory ---------------- */

static void *xcalloc(size_t n, size_t sz) {
    return calloc(n, sz);
}

static char *xstrdup(const char *s) {
    if (!s) return NULL;
    size_t n = strlen(s) + 1;
    char *p = (char *)malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

static int buf_reserve(moq_buf *b, size_t extra) {
    if (b->len + extra <= b->cap) return 0;
    size_t cap = b->cap ? b->cap : 256;
    while (cap < b->len + extra) cap *= 2;
    uint8_t *p = (uint8_t *)realloc(b->p, cap);
    if (!p) return -1;
    b->p = p;
    b->cap = cap;
    return 0;
}

static int buf_append(moq_buf *b, const void *data, size_t len) {
    if (!len) return 0;
    if (buf_reserve(b, len) < 0) return -1;
    memcpy(b->p + b->len, data, len);
    b->len += len;
    return 0;
}

static void buf_consume(moq_buf *b, size_t n) {
    if (n >= b->len) { b->len = 0; return; }
    memmove(b->p, b->p + n, b->len - n);
    b->len -= n;
}

static void buf_free(moq_buf *b) {
    free(b->p);
    b->p = NULL;
    b->len = b->cap = 0;
}

/* ---------------- vi64 / bytes ---------------- */

/* draft-ietf-moq-transport vi64: 1..9 bytes, leading-one length prefix. */
static void put_vi64(moq_buf *b, uint64_t v) {
    size_t n = 1;
    while (n <= 8 && v >= (1ULL << (7 * n))) n++;
    if (n <= 8) {
        size_t lead = n - 1;
        uint8_t mask = (lead == 0) ? 0x00 : (uint8_t)(0xFFu << (8 - lead));
        uint8_t first = (uint8_t)(mask | ((v >> (8 * (n - 1))) &
                                          ((1u << (8 - n)) - 1u)));
        buf_append(b, &first, 1);
    } else {
        uint8_t first = 0xFF;
        buf_append(b, &first, 1);
    }
    for (size_t i = 1; i < n; i++) {
        uint8_t c = (uint8_t)((v >> (8 * (n - 1 - i))) & 0xff);
        buf_append(b, &c, 1);
    }
}

static void put_u16(moq_buf *b, uint16_t v) {
    uint8_t p[2] = { (uint8_t)(v >> 8), (uint8_t)v };
    buf_append(b, p, 2);
}

static void put_bytes(moq_buf *b, const void *p, size_t n) {
    if (p && n) buf_append(b, p, n);
}

static void put_str(moq_buf *b, const char *s) {
    size_t n = s ? strlen(s) : 0;
    put_vi64(b, n);
    if (n) put_bytes(b, s, n);
}

static int get_vi64(const uint8_t *p, size_t len, size_t *off, uint64_t *out) {
    if (*off >= len) return 0;
    uint8_t b0 = p[*off];
    size_t n = 1;
    uint8_t mask = 0x80;
    while (n <= 8 && (b0 & mask)) { n++; mask >>= 1; }
    if (*off + n > len) return 0;
    uint64_t v = 0;
    if (n == 1) {
        v = b0 & 0x7f;
    } else if (n <= 8) {
        v = b0 & ((1u << (8 - n)) - 1u);
        for (size_t i = 1; i < n; i++) v = (v << 8) | p[*off + i];
    } else {
        v = 0;
        for (size_t i = 1; i <= 8; i++) v = (v << 8) | p[*off + i];
    }
    *off += n;
    *out = v;
    return 1;
}

static int get_bytes(const uint8_t *p, size_t len, size_t *off,
                     size_t n, const uint8_t **out) {
    if (*off + n > len) return 0;
    *out = p + *off;
    *off += n;
    return 1;
}

/* ---------------- wire messages ---------------- */

static moq_buf build_setup(void) {
    moq_buf b = {0};
    put_vi64(&b, MOQT_SETUP);
    put_u16(&b, 0);
    return b;
}

static moq_buf build_namespace(const char *app, const char *stream) {
    moq_buf b = {0};
    put_vi64(&b, 2);
    put_str(&b, app);
    put_str(&b, stream);
    return b;
}

static moq_buf build_publish(const char *app, const char *stream,
                             const char *track, uint64_t alias,
                             uint64_t request_id) {
    moq_buf body = {0};
    put_vi64(&body, request_id);
    moq_buf ns = build_namespace(app, stream);
    put_bytes(&body, ns.p, ns.len);
    buf_free(&ns);
    put_str(&body, track);
    put_vi64(&body, alias);
    put_vi64(&body, 0);                 /* no publish params */
    put_vi64(&body, 0);                 /* no subscribe params */

    moq_buf out = {0};
    put_vi64(&out, MOQT_PUBLISH);
    put_u16(&out, (uint16_t)body.len);
    put_bytes(&out, body.p, body.len);
    buf_free(&body);
    return out;
}

static moq_buf build_subscribe(const char *app, const char *stream,
                               const char *track, uint64_t alias,
                               uint64_t request_id) {
    moq_buf body = {0};
    put_vi64(&body, request_id);
    moq_buf ns = build_namespace(app, stream);
    put_bytes(&body, ns.p, ns.len);
    buf_free(&ns);
    put_str(&body, track);
    put_vi64(&body, alias);

    moq_buf out = {0};
    put_vi64(&out, MOQT_SUBSCRIBE);
    put_u16(&out, (uint16_t)body.len);
    put_bytes(&out, body.p, body.len);
    buf_free(&body);
    return out;
}

static moq_buf build_subgroup(uint64_t alias, uint64_t group) {
    moq_buf b = {0};
    put_vi64(&b, SUBGROUP_FLAGS);
    put_vi64(&b, alias);
    put_vi64(&b, group);
    return b;
}

static void put_prop_num(moq_buf *body, uint64_t *prev, uint64_t type,
                         uint64_t value) {
    put_vi64(body, type - *prev);
    *prev = type;
    put_vi64(body, value);
}

static void put_prop_bytes(moq_buf *body, uint64_t *prev, uint64_t type,
                           const void *p, size_t n) {
    put_vi64(body, type - *prev);
    *prev = type;
    put_vi64(body, n);
    put_bytes(body, p, n);
}

static moq_buf build_object(int64_t dts_ms, int64_t cts_ms, int key,
                            const uint8_t *vcfg, size_t vcfg_len,
                            const uint8_t *acfg, size_t acfg_len,
                            const uint8_t *payload, size_t payload_len) {
    uint64_t ts = dts_ms < 0 ? 0 : (uint64_t)dts_ms;
    uint64_t cts = cts_ms < 0 ? 0 : (uint64_t)cts_ms;

    moq_buf props = {0};
    uint64_t prev = 0;
    put_prop_num(&props, &prev, LOC_TIMESCALE, 1000);
    if (key) {
        uint8_t k = 1;
        put_prop_bytes(&props, &prev, LOC_FRAME_MARK, &k, 1);
    }
    if (vcfg && vcfg_len)
        put_prop_bytes(&props, &prev, LOC_VIDEO_CONFIG, vcfg, vcfg_len);
    if (cts)
        put_prop_num(&props, &prev, LOC_COMPOSITION, cts);
    if (acfg && acfg_len)
        put_prop_bytes(&props, &prev, LOC_AUDIO_CONFIG, acfg, acfg_len);
    put_prop_num(&props, &prev, LOC_TIMESTAMP, ts);

    moq_buf out = {0};
    put_vi64(&out, 0);                 /* object id delta */
    put_vi64(&out, props.len);
    put_bytes(&out, props.p, props.len);
    put_vi64(&out, payload_len);
    if (payload_len) put_bytes(&out, payload, payload_len);
    buf_free(&props);
    return out;
}

/* ---------------- JSON catalog ---------------- */

static void json_escape(moq_buf *b, const char *s) {
    for (const unsigned char *p = (const unsigned char *)s; p && *p; p++) {
        if (*p == '"' || *p == '\\') {
            uint8_t esc = '\\';
            buf_append(b, &esc, 1);
            buf_append(b, p, 1);
        } else if (*p < 0x20) {
            char tmp[7];
            snprintf(tmp, sizeof(tmp), "\\u%04x", *p);
            buf_append(b, tmp, 6);
        } else {
            buf_append(b, p, 1);
        }
    }
}

static moq_buf build_catalog(moqt_client_t *c) {
    moq_buf b = {0};
    const char *head = "{\"tracks\":[";
    buf_append(&b, head, strlen(head));

    moqt_stream_t *v = c->video;
    moqt_stream_t *a = c->audio;
    if (v) {
        const char *s = "{\"name\":\"video\",\"namespace\":\"";
        buf_append(&b, s, strlen(s));
        json_escape(&b, c->app);
        buf_append(&b, "/", 1);
        json_escape(&b, c->stream);
        const char *m = "\",\"codec\":\"";
        buf_append(&b, m, strlen(m));
        json_escape(&b, v->codec_name);
        const char *t = "\",\"packaging\":\"loc\"}";
        buf_append(&b, t, strlen(t));
    }
    if (v && a) buf_append(&b, ",", 1);
    if (a) {
        const char *s = "{\"name\":\"audio\",\"namespace\":\"";
        buf_append(&b, s, strlen(s));
        json_escape(&b, c->app);
        buf_append(&b, "/", 1);
        json_escape(&b, c->stream);
        const char *m = "\",\"codec\":\"";
        buf_append(&b, m, strlen(m));
        json_escape(&b, a->codec_name);
        const char *t = "\",\"packaging\":\"loc\"}";
        buf_append(&b, t, strlen(t));
    }
    buf_append(&b, "]}", 2);
    return b;
}

/* ---------------- stream helpers ---------------- */

static moqt_stream_t *find_stream_wt(moqt_client_t *c, wt_stream_t *wt) {
    for (moqt_stream_t *s = c->streams; s; s = s->next)
        if (s->wt == wt) return s;
    return NULL;
}

static moqt_stream_t *new_stream(moqt_client_t *c) {
    moqt_stream_t *s = (moqt_stream_t *)xcalloc(1, sizeof(*s));
    if (!s) return NULL;
    s->client = c;
    s->next = c->streams;
    c->streams = s;
    return s;
}

static void free_stream(moqt_stream_t *s) {
    if (!s) return;
    free(s->config);
    buf_free(&s->rx);
    free(s);
}

static void set_track_for_alias(moqt_stream_t *s, uint64_t alias) {
    s->alias = alias;
    if (alias == ALIAS_VIDEO) {
        s->codec = MOQT_CODEC_H264;
        strncpy(s->track, "video", sizeof(s->track) - 1);
        strncpy(s->codec_name, "avc1.64001f", sizeof(s->codec_name) - 1);
    } else if (alias == ALIAS_AUDIO) {
        s->codec = MOQT_CODEC_AAC;
        strncpy(s->track, "audio", sizeof(s->track) - 1);
        strncpy(s->codec_name, "mp4a.40.2", sizeof(s->codec_name) - 1);
    }
}

typedef struct {
    moqt_stream_t *stream;
    moqt_write_cb  cb;
    void          *user;
    size_t         bytes;
} moqt_write_ctx;

static void on_write_done(wt_stream_t *st, int ret, void *user) {
    (void)st;
    moqt_write_ctx *ctx = (moqt_write_ctx *)user;
    if (!ctx) return;
    moqt_stream_t *s = ctx->stream;
    if (s) {
        if (s->inflight >= ctx->bytes) s->inflight -= ctx->bytes;
        else s->inflight = 0;
    }
    moqt_result_t status = (ret == 0) ? MOQT_OK
                          : (ret == -2) ? MOQT_ERR_TIMEOUT
                                        : MOQT_ERR_WRITE;
    if (ctx->cb && s) ctx->cb(s, status, ret == 0 ? ctx->bytes : 0, ctx->user);
    free(ctx);
}

static void send_buf(moqt_client_t *c, wt_stream_t *st, moq_buf *b) {
    (void)c;
    if (!st || !b || !b->len) return;
    /* The WT API copies the bytes into its task queue. */
    wt_stream_write_cb(st, b->p, b->len, NULL, NULL, 5000);
}

/* ---------------- publisher ---------------- */

static void pub_open_next(moqt_client_t *c) {
    if (!c->wt || c->pub_opened >= PUB_DONE) return;
    int slot = c->pub_opened;
    if (slot == PUB_CONTROL || slot >= PUB_DATA_CATALOG)
        wt_client_open_uni_stream(c->wt);
    else
        wt_client_open_stream(c->wt);
}

static void notify_stream_ready(moqt_client_t *c, moqt_stream_t *s) {
    if (!c->cb.on_stream_ready) return;
    moqt_stream_info_t info;
    info.codec = s->codec;
    info.config = s->config;
    info.config_len = s->config_len;
    info.track_name = s->track;
    c->cb.on_stream_ready(c, s, &info, c->user);
}

static void pub_handle_open(moqt_client_t *c, int slot, wt_stream_t *st) {
    if (slot == PUB_CONTROL) {
        c->control = st;
        moq_buf b = build_setup();
        send_buf(c, st, &b);
        buf_free(&b);
        return;
    }
    if (slot == PUB_REQ_CATALOG) {
        moq_buf b = build_publish(c->app, c->stream, "catalog",
                                  ALIAS_CATALOG, 0);
        send_buf(c, st, &b);
        buf_free(&b);
        return;
    }
    if (slot == PUB_REQ_VIDEO) {
        moq_buf b = build_publish(c->app, c->stream, "video",
                                  ALIAS_VIDEO, 2);
        send_buf(c, st, &b);
        buf_free(&b);
        return;
    }
    if (slot == PUB_REQ_AUDIO) {
        moq_buf b = build_publish(c->app, c->stream, "audio",
                                  ALIAS_AUDIO, 4);
        send_buf(c, st, &b);
        buf_free(&b);
        return;
    }
    if (slot == PUB_DATA_CATALOG) {
        moq_buf h = build_subgroup(ALIAS_CATALOG, 0);
        send_buf(c, st, &h);
        buf_free(&h);
        moq_buf json = build_catalog(c);
        moq_buf obj = build_object(0, 0, 0, NULL, 0, NULL, 0,
                                   json.p, json.len);
        send_buf(c, st, &obj);
        buf_free(&json);
        buf_free(&obj);
        return;
    }
    if (slot == PUB_DATA_VIDEO && c->video) {
        c->video->wt = st;
        c->video->ready = 1;
        moq_buf h = build_subgroup(ALIAS_VIDEO, 0);
        send_buf(c, st, &h);
        buf_free(&h);
        notify_stream_ready(c, c->video);
        return;
    }
    if (slot == PUB_DATA_AUDIO && c->audio) {
        c->audio->wt = st;
        c->audio->ready = 1;
        moq_buf h = build_subgroup(ALIAS_AUDIO, 0);
        send_buf(c, st, &h);
        buf_free(&h);
        notify_stream_ready(c, c->audio);
        return;
    }
}

/* ---------------- subscriber ---------------- */

typedef struct {
    uint64_t dts;
    uint64_t cts;
    int key;
    const uint8_t *vcfg;
    size_t vcfg_len;
    const uint8_t *acfg;
    size_t acfg_len;
    const uint8_t *payload;
    size_t payload_len;
    size_t consumed;
} obj_t;

static int parse_props(const uint8_t *p, size_t len, obj_t *o) {
    size_t off = 0;
    uint64_t prev = 0;
    while (off < len) {
        uint64_t delta = 0, type = 0;
        if (!get_vi64(p, len, &off, &delta)) return -1;
        type = prev + delta;
        prev = type;
        if ((type & 1) == 0) {
            uint64_t v = 0;
            if (!get_vi64(p, len, &off, &v)) return -1;
            if (type == LOC_TIMESTAMP) o->dts = v;
            else if (type == LOC_COMPOSITION) o->cts = v;
        } else {
            uint64_t n = 0;
            const uint8_t *raw = NULL;
            if (!get_vi64(p, len, &off, &n)) return -1;
            if (!get_bytes(p, len, &off, (size_t)n, &raw)) return -1;
            if (type == LOC_FRAME_MARK) {
                if (n > 0 && (raw[0] & 1)) o->key = 1;
            } else if (type == LOC_VIDEO_CONFIG) {
                o->vcfg = raw; o->vcfg_len = (size_t)n;
            } else if (type == LOC_AUDIO_CONFIG) {
                o->acfg = raw; o->acfg_len = (size_t)n;
            }
        }
    }
    return 0;
}

/* 0 = incomplete, 1 = complete, -1 = malformed */
static int parse_object(const uint8_t *p, size_t len, int has_props, obj_t *o) {
    size_t off = 0;
    memset(o, 0, sizeof(*o));
    uint64_t delta = 0;
    if (!get_vi64(p, len, &off, &delta)) return 0;
    if (has_props) {
        uint64_t plen = 0;
        const uint8_t *props = NULL;
        if (!get_vi64(p, len, &off, &plen)) return 0;
        if (!get_bytes(p, len, &off, (size_t)plen, &props)) return 0;
        if (parse_props(props, (size_t)plen, o) < 0) return -1;
    }
    uint64_t payload_len = 0;
    if (!get_vi64(p, len, &off, &payload_len)) return 0;
    if (payload_len == 0) {
        uint64_t status = 0;
        if (!get_vi64(p, len, &off, &status)) return 0;
    } else {
        const uint8_t *payload = NULL;
        if (!get_bytes(p, len, &off, (size_t)payload_len, &payload)) return 0;
        o->payload = payload;
        o->payload_len = (size_t)payload_len;
    }
    o->consumed = off;
    return 1;
}

/* 0 = incomplete, 1 = complete, -1 = malformed */
static int parse_subgroup(const uint8_t *p, size_t len,
                          uint64_t *alias, uint64_t *group,
                          int *has_props, size_t *consumed) {
    size_t off = 0;
    uint64_t flags = 0, a = 0, g = 0;
    if (!get_vi64(p, len, &off, &flags)) return 0;
    if (!get_vi64(p, len, &off, &a)) return 0;
    if (!get_vi64(p, len, &off, &g)) return 0;
    uint64_t id_mode = (flags >> 1) & 0x03;
    if (id_mode == 0x02) {
        uint64_t sg = 0;
        if (!get_vi64(p, len, &off, &sg)) return 0;
    }
    if ((flags & 0x20) == 0) {
        if (off >= len) return 0;
        off++; /* priority byte */
    }
    *alias = a;
    *group = g;
    *has_props = (flags & 0x01) != 0;
    *consumed = off;
    return 1;
}

static void ensure_sub_stream_link(moqt_client_t *c, moqt_stream_t *s) {
    if (s->alias == ALIAS_VIDEO && !c->video) c->video = s;
    if (s->alias == ALIAS_AUDIO && !c->audio) c->audio = s;
}

static void sub_parse_stream(moqt_stream_t *s) {
    moqt_client_t *c = s->client;
    for (;;) {
        if (!s->header_done) {
            uint64_t alias = 0, group = 0;
            int has_props = 0;
            size_t used = 0;
            int r = parse_subgroup(s->rx.p, s->rx.len, &alias, &group,
                                   &has_props, &used);
            if (r == 0) return;
            if (r < 0) { s->rx.len = 0; return; }
            buf_consume(&s->rx, used);
            if (alias == ALIAS_VIDEO || alias == ALIAS_AUDIO) {
                set_track_for_alias(s, alias);
                ensure_sub_stream_link(c, s);
            } else {
                s->alias = alias;
            }
            s->header_done = 1;
            s->has_props = has_props;
            s->group_id = group;
        }

        if (s->alias != ALIAS_VIDEO && s->alias != ALIAS_AUDIO) {
            /* catalog / unknown track: consume objects without callbacks */
            obj_t o;
            int r = parse_object(s->rx.p, s->rx.len, s->has_props, &o);
            if (r == 0) return;
            if (r < 0) { s->rx.len = 0; return; }
            buf_consume(&s->rx, o.consumed);
            continue;
        }

        obj_t o;
        int r = parse_object(s->rx.p, s->rx.len, s->has_props, &o);
        if (r == 0) return;
        if (r < 0) { s->rx.len = 0; return; }

        if (!s->announced) {
            if (o.vcfg && o.vcfg_len) {
                uint8_t *cp = (uint8_t *)malloc(o.vcfg_len);
                if (cp) {
                    memcpy(cp, o.vcfg, o.vcfg_len);
                    free(s->config);
                    s->config = cp;
                    s->config_len = o.vcfg_len;
                }
            } else if (o.acfg && o.acfg_len) {
                uint8_t *cp = (uint8_t *)malloc(o.acfg_len);
                if (cp) {
                    memcpy(cp, o.acfg, o.acfg_len);
                    free(s->config);
                    s->config = cp;
                    s->config_len = o.acfg_len;
                }
            }
            s->announced = 1;
            notify_stream_ready(c, s);
        } else {
            /* a later config update is still useful to the decoder */
            if (o.vcfg && o.vcfg_len) {
                uint8_t *cp = (uint8_t *)malloc(o.vcfg_len);
                if (cp) {
                    memcpy(cp, o.vcfg, o.vcfg_len);
                    free(s->config);
                    s->config = cp;
                    s->config_len = o.vcfg_len;
                }
            } else if (o.acfg && o.acfg_len) {
                uint8_t *cp = (uint8_t *)malloc(o.acfg_len);
                if (cp) {
                    memcpy(cp, o.acfg, o.acfg_len);
                    free(s->config);
                    s->config = cp;
                    s->config_len = o.acfg_len;
                }
            }
        }

        if (o.payload_len > 0 && c->cb.on_data) {
            /* Callback must run before the buffer is compacted: payload
             * points into s->rx. */
            c->cb.on_data(c, s, o.payload, o.payload_len,
                          (int64_t)o.dts, (int64_t)(o.dts + o.cts),
                          o.key, c->user);
        }
        buf_consume(&s->rx, o.consumed);
    }
}

static void sub_on_incoming(moqt_client_t *c, wt_stream_t *wt,
                            const uint8_t *data, size_t len) {
    moqt_stream_t *s = find_stream_wt(c, wt);
    if (!s) {
        s = new_stream(c);
        if (!s) return;
        s->wt = wt;
    }
    if (buf_append(&s->rx, data, len) < 0) return;
    sub_parse_stream(s);
}

/* ---------------- WebTransport callbacks ---------------- */

static void on_connect_timeout(uv_timer_t *t) {
    moqt_client_t *c = (moqt_client_t *)t->data;
    if (!c || c->state != C_CONNECTING || c->connect_reported) return;
    c->connect_reported = 1;
    c->close_reported = 1;
    c->state = C_CLOSED;
    uv_timer_stop(&c->connect_timer);
    if (c->wt) wt_client_close(c->wt);
    if (c->cb.on_connected)
        c->cb.on_connected(c, MOQT_ERR_TIMEOUT, "connect timeout", c->user);
}

static void on_wt_connected(wt_client_t *cli, int status, void *user) {
    (void)cli;
    moqt_client_t *c = (moqt_client_t *)user;
    if (!c) return;
    if (c->connect_reported) return;
    c->connect_reported = 1;
    uv_timer_stop(&c->connect_timer);
    if (status != 0) {
        c->state = C_CLOSED;
        c->close_reported = 1;
        if (c->cb.on_connected)
            c->cb.on_connected(c, MOQT_ERR_CONNECT,
                               "WebTransport handshake failed", c->user);
        return;
    }
    c->state = C_CONNECTED;
    if (c->cb.on_connected) c->cb.on_connected(c, MOQT_OK, "", c->user);
    if (c->dir == MOQT_DIRECTION_PUBLISH) {
        c->pub_opened = 0;
        pub_open_next(c);
    } else {
        c->sub_step = 0;
        wt_client_open_uni_stream(c->wt);
    }
}

static void on_wt_stream_open(wt_client_t *cli, wt_stream_t *st, void *user) {
    (void)cli;
    moqt_client_t *c = (moqt_client_t *)user;
    if (!c) return;
    if (c->dir == MOQT_DIRECTION_PUBLISH) {
        int slot = c->pub_opened;
        if (slot >= PUB_DONE) return;
        pub_handle_open(c, slot, st);
        c->pub_opened++;
        pub_open_next(c);
        return;
    }
    if (c->sub_step == 0) {
        c->control = st;
        moq_buf b = build_setup();
        send_buf(c, st, &b);
        buf_free(&b);
        c->sub_step = 1;
        wt_client_open_stream(c->wt);
        return;
    }
    if (c->sub_step == 1) {
        c->req = st;
        moq_buf v = build_subscribe(c->app, c->stream, "video",
                                    ALIAS_VIDEO, 1);
        send_buf(c, st, &v);
        buf_free(&v);
        moq_buf a = build_subscribe(c->app, c->stream, "audio",
                                    ALIAS_AUDIO, 2);
        send_buf(c, st, &a);
        buf_free(&a);
        c->sub_step = 2;
    }
}

static void on_wt_stream_data(wt_client_t *cli, wt_stream_t *st,
                              const uint8_t *data, size_t len, void *user) {
    (void)cli;
    moqt_client_t *c = (moqt_client_t *)user;
    if (!c || c->dir != MOQT_DIRECTION_SUBSCRIBE) return;
    if (st == c->req || st == c->control) return; /* SUBSCRIBE_OK */
    sub_on_incoming(c, st, data, len);
}

static void on_wt_stream_close(wt_client_t *cli, wt_stream_t *st, void *user) {
    (void)cli;
    moqt_client_t *c = (moqt_client_t *)user;
    if (!c) return;
    for (moqt_stream_t *s = c->streams; s; s = s->next) {
        if (s->wt == st) s->ready = 0;
    }
}

static void on_wt_close(wt_client_t *cli, int err, void *user) {
    (void)cli;
    moqt_client_t *c = (moqt_client_t *)user;
    if (!c) return;
    uv_timer_stop(&c->connect_timer);
    c->state = C_CLOSED;
    const char *reason = c->close_reason[0] ? c->close_reason
                                            : "session closed";
    if (!c->connect_reported) {
        c->connect_reported = 1;
        c->close_reported = 1;
        if (c->cb.on_connected)
            c->cb.on_connected(c, MOQT_ERR_CONNECT, reason, c->user);
        return;
    }
    if (!c->close_reported) {
        c->close_reported = 1;
        if (c->cb.on_closed)
            c->cb.on_closed(c, err == 0 ? MOQT_OK : MOQT_ERR_CLOSED,
                            reason, c->user);
    }
}

/* ---------------- public API ---------------- */

moqt_client_t *moqt_client_new(uv_loop_t *loop,
                               const moqt_callbacks_t *callbacks,
                               void *user) {
    if (!loop || !callbacks) return NULL;
    moqt_client_t *c = (moqt_client_t *)xcalloc(1, sizeof(*c));
    if (!c) return NULL;
    c->loop = loop;
    c->cb = *callbacks;
    c->user = user;
    c->state = C_IDLE;
    c->connect_timeout_ms = 10000;
    uv_timer_init(loop, &c->connect_timer);
    c->connect_timer.data = c;
    return c;
}

moqt_stream_t *moqt_client_add_stream(moqt_client_t *c,
                                      const moqt_stream_config_t *cfg) {
    if (!c || !cfg || c->state != C_IDLE) return NULL;
    if (cfg->codec != MOQT_CODEC_H264 && cfg->codec != MOQT_CODEC_AAC)
        return NULL;
    if (cfg->codec == MOQT_CODEC_H264 && c->video) return NULL;
    if (cfg->codec == MOQT_CODEC_AAC && c->audio) return NULL;
    if (cfg->config_len > 0 && !cfg->config) return NULL;

    moqt_stream_t *s = new_stream(c);
    if (!s) return NULL;
    s->codec = cfg->codec;
    if (cfg->codec == MOQT_CODEC_H264) {
        strncpy(s->track, "video", sizeof(s->track) - 1);
        strncpy(s->codec_name,
                cfg->codec_name ? cfg->codec_name : "avc1.64001f",
                sizeof(s->codec_name) - 1);
        s->alias = ALIAS_VIDEO;
        c->video = s;
    } else {
        strncpy(s->track, "audio", sizeof(s->track) - 1);
        strncpy(s->codec_name,
                cfg->codec_name ? cfg->codec_name : "mp4a.40.2",
                sizeof(s->codec_name) - 1);
        s->alias = ALIAS_AUDIO;
        c->audio = s;
    }
    if (cfg->config_len) {
        s->config = (uint8_t *)malloc(cfg->config_len);
        if (!s->config) return NULL;
        memcpy(s->config, cfg->config, cfg->config_len);
        s->config_len = cfg->config_len;
    }
    return s;
}

moqt_result_t moqt_client_connect_ex(moqt_client_t *c,
                                     const char *authority_host,
                                     const char *connect_ip,
                                     int port, const char *path,
                                     const char *app, const char *stream,
                                     moqt_direction_t direction,
                                     int connect_timeout_ms) {
    if (!c || !authority_host || !connect_ip || !app || !stream)
        return MOQT_ERR_INVALID;
    if (direction != MOQT_DIRECTION_PUBLISH &&
        direction != MOQT_DIRECTION_SUBSCRIBE)
        return MOQT_ERR_INVALID;
    if (direction == MOQT_DIRECTION_PUBLISH && (!c->video || !c->audio))
        return MOQT_ERR_INVALID;
    if (c->state != C_IDLE) return MOQT_ERR_STATE;

    c->host = xstrdup(authority_host);
    c->path = xstrdup(path && path[0] ? path : "/moq");
    c->app = xstrdup(app);
    c->stream = xstrdup(stream);
    c->port = port;
    c->dir = direction;
    if (!c->host || !c->path || !c->app || !c->stream) return -1;

    wt_callbacks_t wcb;
    memset(&wcb, 0, sizeof(wcb));
    wcb.on_connect = on_wt_connected;
    wcb.on_stream_open = on_wt_stream_open;
    wcb.on_stream_data = on_wt_stream_data;
    wcb.on_stream_close = on_wt_stream_close;
    wcb.on_close = on_wt_close;
    wcb.user_data = c;

    c->wt = wt_client_new(c->loop, &wcb);
    if (!c->wt) return MOQT_ERR_CONNECT;
    c->state = C_CONNECTING;
    c->connect_reported = 0;
    c->close_reported = 0;
    c->connect_timeout_ms = connect_timeout_ms > 0 ? connect_timeout_ms : 10000;
    uv_timer_start(&c->connect_timer, on_connect_timeout,
                   (uint64_t)c->connect_timeout_ms, 0);
    wt_client_connect_path_ex(c->wt, c->host, connect_ip, c->port, c->path);
    return MOQT_OK;
}

moqt_result_t moqt_client_connect(moqt_client_t *c,
                                  const char *host, int port, const char *path,
                                  const char *app, const char *stream,
                                  moqt_direction_t direction,
                                  int connect_timeout_ms) {
    return moqt_client_connect_ex(c, host, host, port, path, app, stream,
                                  direction, connect_timeout_ms);
}

moqt_result_t moqt_stream_write_ex(moqt_stream_t *s,
                                   const uint8_t *data, size_t len,
                                   int64_t dts_ms, int64_t pts_ms, int keyframe,
                                   moqt_write_cb cb, void *user) {
    if (!s || !s->client || !s->ready || !s->wt) return MOQT_ERR_STATE;
    if (len && !data) return MOQT_ERR_INVALID;
    if (s->high_watermark && s->inflight + len > s->high_watermark)
        return MOQT_ERR_AGAIN;

    int64_t cts = pts_ms - dts_ms;
    if (cts < 0) cts = 0;
    int key = (s->codec == MOQT_CODEC_H264) ? (keyframe ? 1 : 0) : 0;

    const uint8_t *vcfg = NULL, *acfg = NULL;
    size_t vcfg_len = 0, acfg_len = 0;
    if (!s->config_sent && s->config_len) {
        if (s->codec == MOQT_CODEC_H264) {
            vcfg = s->config; vcfg_len = s->config_len;
        } else {
            acfg = s->config; acfg_len = s->config_len;
        }
    }

    moq_buf obj = build_object(dts_ms, cts, key,
                               vcfg, vcfg_len, acfg, acfg_len,
                               data, len);

    moqt_write_ctx *ctx = (moqt_write_ctx *)calloc(1, sizeof(*ctx));
    if (!ctx) {
        buf_free(&obj);
        return MOQT_ERR_NOMEM;
    }
    ctx->stream = s;
    ctx->cb = cb;
    ctx->user = user;
    ctx->bytes = len;
    s->config_sent = 1;
    s->inflight += len;
    wt_stream_write_cb(s->wt, obj.p, obj.len, on_write_done, ctx, 5000);
    buf_free(&obj);
    return MOQT_OK;
}

moqt_result_t moqt_stream_write(moqt_stream_t *s,
                                const uint8_t *data, size_t len,
                                int64_t dts_ms, int64_t pts_ms, int keyframe) {
    return moqt_stream_write_ex(s, data, len, dts_ms, pts_ms, keyframe,
                                NULL, NULL);
}

size_t moqt_stream_inflight_bytes(const moqt_stream_t *s) {
    return s ? s->inflight : 0;
}

moqt_result_t moqt_stream_set_high_watermark(moqt_stream_t *s, size_t bytes) {
    if (!s) return MOQT_ERR_INVALID;
    s->high_watermark = bytes;
    return MOQT_OK;
}

moqt_codec_t moqt_stream_codec(const moqt_stream_t *s) {
    return s ? s->codec : MOQT_CODEC_H264;
}

const char *moqt_stream_track_name(const moqt_stream_t *s) {
    return s ? s->track : "";
}

const uint8_t *moqt_stream_config(const moqt_stream_t *s, size_t *len) {
    if (len) *len = s ? s->config_len : 0;
    return s ? s->config : NULL;
}

void moqt_client_close(moqt_client_t *c) {
    if (!c) return;
    if (c->closing) return;
    c->closing = 1;
    snprintf(c->close_reason, sizeof(c->close_reason), "closed by user");
    if (c->wt) wt_client_close(c->wt);
}

void moqt_client_free(moqt_client_t *c) {
    if (!c) return;
    uv_timer_stop(&c->connect_timer);
    moqt_stream_t *s = c->streams;
    while (s) {
        moqt_stream_t *n = s->next;
        free_stream(s);
        s = n;
    }
    free(c->host);
    free(c->path);
    free(c->app);
    free(c->stream);
    free(c);
}
