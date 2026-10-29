#ifndef MOQ_TRANSPORT_CLIENT_H
#define MOQ_TRANSPORT_CLIENT_H

/* ============================================================
 * moq-transport client SDK
 *
 * Scope: live H.264 + AAC over moq-transport (draft-22) carried by
 * WebTransport/HTTP3.  One client = one broadcast (app/stream) and one
 * direction: publish or subscribe.
 *
 * Threading: callbacks run on the uv loop thread.  Call the SDK from that
 * thread; `moqt_client_close()` is safe to call from another thread.
 * ============================================================ */

#include <stddef.h>
#include <stdint.h>
#include <uv.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct moqt_client moqt_client_t;
typedef struct moqt_stream moqt_stream_t;

typedef enum {
    MOQT_OK          = 0,
    MOQT_ERR_INVALID = -1,   /* bad argument */
    MOQT_ERR_STATE   = -2,   /* wrong lifecycle state */
    MOQT_ERR_CONNECT = -3,   /* WebTransport/QUIC handshake failed */
    MOQT_ERR_TIMEOUT = -4,   /* connect or write timed out */
    MOQT_ERR_CLOSED  = -5,   /* session/stream closed */
    MOQT_ERR_AGAIN   = -6,   /* backpressure: retry/drop later */
    MOQT_ERR_WRITE   = -7,   /* write failed */
    MOQT_ERR_NOMEM   = -8
} moqt_result_t;

typedef enum {
    MOQT_CODEC_H264 = 0,
    MOQT_CODEC_AAC  = 1
} moqt_codec_t;

typedef enum {
    MOQT_DIRECTION_PUBLISH   = 0,
    MOQT_DIRECTION_SUBSCRIBE = 1
} moqt_direction_t;

/* Publisher: one entry per track (video + audio).  `config` is avcC for
 * H.264 and AudioSpecificConfig for AAC; it is copied by add_stream(). */
typedef struct {
    moqt_codec_t  codec;
    const uint8_t *config;
    size_t         config_len;
    const char    *codec_name;   /* optional, e.g. "avc1.64001f" */
} moqt_stream_config_t;

/* Passed to on_stream_ready().  Pointers are owned by the SDK and only
 * valid for the duration of the callback. */
typedef struct {
    moqt_codec_t   codec;
    const uint8_t *config;
    size_t         config_len;
    const char    *track_name;
} moqt_stream_info_t;

typedef struct {
    /* Fires exactly once.  status == MOQT_OK means the WebTransport session
     * is ready; otherwise `reason` explains the failure. */
    void (*on_connected)(moqt_client_t *c, moqt_result_t status,
                         const char *reason, void *user);

    /* Publisher: after PUBLISH and its data stream are ready.
     * Subscriber: when the first object for a track is decoded; `config`
     * may still be NULL if the first object carried no codec config. */
    void (*on_stream_ready)(moqt_client_t *c, moqt_stream_t *s,
                            const moqt_stream_info_t *info, void *user);

    /* Subscriber only.  `data` is AVCC H.264 / raw AAC; dts/pts are in ms.
     * The buffer is only valid during the callback. */
    void (*on_data)(moqt_client_t *c, moqt_stream_t *s,
                    const uint8_t *data, size_t len,
                    int64_t dts_ms, int64_t pts_ms, int keyframe,
                    void *user);

    /* Fires after an established session closes; not called for a failed
     * connect (that is reported by on_connected). */
    void (*on_closed)(moqt_client_t *c, moqt_result_t status,
                      const char *reason, void *user);
} moqt_callbacks_t;

/* Optional write-completion callback.  QUIC stream writes are all-or-nothing:
 * on MOQT_OK all `len` bytes were ACKed; on failure `bytes` is 0. */
typedef void (*moqt_write_cb)(moqt_stream_t *s, moqt_result_t status,
                              size_t bytes, void *user);

/* Create a client.  The callbacks struct is copied. */
moqt_client_t *moqt_client_new(uv_loop_t *loop,
                               const moqt_callbacks_t *callbacks,
                               void *user);

/* Publisher only: add a video/audio track before connect().  Returns NULL
 * for duplicate codecs or invalid arguments.  The config bytes are copied. */
moqt_stream_t *moqt_client_add_stream(moqt_client_t *c,
                                      const moqt_stream_config_t *cfg);

/* Connect and start the session.  `path` is the server WebTransport path
 * (normally "/moq"); app/stream form the MoQ namespace and must be non-empty.
 * For publish, call add_stream() for video and audio before this.
 * For subscribe, the SDK creates the video/audio streams from incoming data.
 * Returns MOQT_OK if the async connect was queued; the final result arrives
 * through on_connected(). */
moqt_result_t moqt_client_connect(moqt_client_t *c,
                        const char *host, int port, const char *path,
                        const char *app, const char *stream,
                        moqt_direction_t direction,
                        int connect_timeout_ms);

/* Same as moqt_client_connect(), but `authority_host` is kept for the
 * WebTransport :authority (and future TLS server name), while `connect_ip`
 * (an IPv4 literal) is used for the QUIC socket.  Typical use:
 *   uv_getaddrinfo(AF_INET, host) -> connect_ip
 *   moqt_client_connect_ex(c, host, connect_ip, ...)
 * For a literal IPv4 host, both arguments can be the same string. */
moqt_result_t moqt_client_connect_ex(moqt_client_t *c,
                        const char *authority_host,
                        const char *connect_ip,
                        int port, const char *path,
                        const char *app, const char *stream,
                        moqt_direction_t direction,
                        int connect_timeout_ms);

/* Publisher: write one media object.  dts/pts are milliseconds; pts may be
 * < dts only if the caller wants composition offset 0.  `keyframe` is used
 * for H.264 frame-mark.  MOQT_OK means queued, not ACKed; use _ex() for a
 * completion callback. */
moqt_result_t moqt_stream_write(moqt_stream_t *s,
                                const uint8_t *data, size_t len,
                                int64_t dts_ms, int64_t pts_ms, int keyframe);

/* Same as moqt_stream_write(), plus a completion callback.  `cb` may be NULL;
 * inflight accounting still happens.  Returns MOQT_ERR_AGAIN if the stream's
 * high watermark would be exceeded. */
moqt_result_t moqt_stream_write_ex(moqt_stream_t *s,
                                   const uint8_t *data, size_t len,
                                   int64_t dts_ms, int64_t pts_ms, int keyframe,
                                   moqt_write_cb cb, void *user);

/* Payload bytes queued/inflight for this stream (not ACKed yet). */
size_t moqt_stream_inflight_bytes(const moqt_stream_t *s);

/* 0 disables the limit.  When inflight + new payload > limit, writes return
 * MOQT_ERR_AGAIN; the caller decides whether to drop (e.g. non-key video). */
moqt_result_t moqt_stream_set_high_watermark(moqt_stream_t *s, size_t bytes);

/* Optional accessors; valid for the lifetime of the stream. */
moqt_codec_t moqt_stream_codec(const moqt_stream_t *s);
const char *moqt_stream_track_name(const moqt_stream_t *s);
const uint8_t *moqt_stream_config(const moqt_stream_t *s, size_t *len);

/* Close the WebTransport session.  on_closed() will fire. */
void moqt_client_close(moqt_client_t *c);

/* Free the client after close/loop shutdown.  Does not free the loop. */
void moqt_client_free(moqt_client_t *c);

#ifdef __cplusplus
}
#endif

#endif /* MOQ_TRANSPORT_CLIENT_H */
