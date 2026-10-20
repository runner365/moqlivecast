# Writing a push client with the moq-lite C API

**English** | [简体中文](how_to_use_moq_lite_c_api_CN.md)

This document is for **C/C++ programs that push H.264 + AAC into `moqlivecast`**:
from connecting to writing frames out, with every function in
`moqlite/moq_lite_client.h` explained.

Source: `moqlite/moq_lite_client.{h,c}` (session state machine) plus
`moqlite/moq_lite_wire.{h,c}` (wire codec, compiles and self-tests standalone).
A complete working reference implementation lives in
`tools/flv2moqlite_client/` (reads an FLV file → pushes it paced by dts).

> **The protocol version is `moq-lite-06`, carried over raw QUIC** (ALPN
> `moq-lite-06`). That path and the browser's WebTransport path (`h3`) are **two
> separate bindings**; the server splits them by ALPN on a single UDP port. This
> library only does the raw-QUIC one — it does **not** offer `h3`, because offering a
> protocol you can't speak produces "connected, then nothing happens" (see
> *Troubleshooting* below).

---

## 1. The shape of it

The library is **callback-driven with integer handles** (modeled on libmoq's C API).
What you get — `moq_lite_session_t`, `moq_lite_broadcast_t`, `moq_lite_track_t` — are
`uint32_t` **handles**, not pointers. You cannot dereference a freed object.
**0 always means "invalid".**

**The event loop is yours.** The library neither picks a loop nor wraps `uv_run`: you
pass in a `uv_loop_t *`, you run it, and you decide when to stop (that loop very
likely also carries your own timers, signals, and other connections).

Pushing has a fixed four-step order:

```
① moq_lite_session_new()                 create the session (hand it your loop)
   moq_lite_session_set_role(PUBLISHER)    ← must happen BEFORE connect
   moq_lite_session_connect()              connect (result arrives via on_connected)

② inside on_connected: moq_lite_publish()        declare an outgoing broadcast
                       moq_lite_publish_track()  declare a track (also fills the catalog)

③ moq_lite_track_frame()                 write frames by timestamp (from your timer/source)

④ moq_lite_track_end() / moq_lite_session_close()   finish
```

**The catalog and the grouping are the library's job — do not build them yourself.**
You supply the codec parameters (codec string + raw avcC/ASC bytes + dimensions or
sample rate); the library generates `catalog.json`, answers the peer's subscriptions,
and decides how each track is split into groups. You just feed frames.

---

## 2. Minimal working skeleton

```c
#include "moq_lite_client.h"
#include <uv.h>

static uv_loop_t        *g_loop;
static moq_lite_track_t  g_video, g_audio;

/* Your raw avcC / AudioSpecificConfig bytes (from SPS+PPS and the ASC) */
extern const uint8_t *g_avcc;  extern size_t g_avcc_len;
extern const uint8_t *g_asc;   extern size_t g_asc_len;

static void on_close(moq_lite_session_t s, int err, const char *reason, void *user) {
    (void)s; (void)user;
    fprintf(stderr, "session closed: %d (%s)\n", err, reason ? reason : "");
    uv_stop(g_loop);
}

static void on_connected(moq_lite_session_t s, int status, void *user) {
    (void)user;
    if (status != 0) {
        fprintf(stderr, "connect failed: %d (%s)\n", status,
                moq_lite_strerror(status));
        uv_stop(g_loop);
        return;
    }

    /* ① declare the outgoing broadcast. `path` is the on-wire broadcast name. */
    moq_lite_broadcast_t bc = moq_lite_publish(s, "my_stream.hang");
    if (!bc) { fprintf(stderr, "publish failed\n"); return; }

    /* ② video track. The codec string must start with "avc1."; config is the
     *    RAW avcC bytes. */
    moq_lite_rendition_t v = {0};
    v.name = "0.h264";  v.codec = "avc1.42c01e";
    v.config = g_avcc;  v.config_len = g_avcc_len;
    v.width = 640;      v.height = 360;
    g_video = moq_lite_publish_track(bc, /*is_video=*/1, &v);

    /* ③ audio track. The codec string must start with "mp4a.40."; config is the
     *    RAW AudioSpecificConfig. */
    moq_lite_rendition_t a = {0};
    a.name = "1.aac";   a.codec = "mp4a.40.2";
    a.config = g_asc;   a.config_len = g_asc_len;
    a.sample_rate = 48000;  a.channels = 1;
    g_audio = moq_lite_publish_track(bc, /*is_video=*/0, &a);

    printf("publishing, waiting for subscribers...\n");
    /* This is the point where you may start feeding frames (next section). */
}

int main(void) {
    g_loop = uv_default_loop();

    moq_lite_session_t s = moq_lite_session_new(g_loop);
    if (!s) return 1;

    /* ⚠️ Must be before connect — it goes into SETUP's Role parameter. */
    moq_lite_session_set_role(s, MOQ_LITE_ROLE_PUBLISHER);

    moq_lite_session_cb_t cb = {0};
    cb.on_connected = on_connected;
    cb.on_close     = on_close;
    /* host accepts a domain or an IP (resolution is IPv4-only) — see the API
     * reference. */
    moq_lite_session_connect(s, "moql://relay.example.com:4433/anon", &cb);

    uv_run(g_loop, UV_RUN_DEFAULT);   /* the loop is yours */
    return 0;
}
```

---

## 3. Writing frames

Once you hold a `moq_lite_track_t`, **you** call `moq_lite_track_frame()` at the right
moments. The library does not pace for you — when to send a frame is your call
(for real-time push, the `-re`-style dts-paced approach lives in
`tools/flv2moqlite_client/main.cpp`, in `PumpVideo`/`PumpAudio` driven by a `uv_timer`).

```c
/* Call this every time your encoder emits a frame. **Must be on the loop thread**
 * (typically your timer or capture callback). */
void on_encoded_video(uint64_t dts_us, const uint8_t *avcc, size_t len, int key) {
    moq_lite_track_frame(g_video, dts_us, avcc, len, key);
}
void on_encoded_audio(uint64_t dts_us, const uint8_t *aac, size_t len) {
    moq_lite_track_frame(g_audio, dts_us, aac, len, 0);
}
```

Three argument rules; getting any of them wrong fails **silently**:

| Argument | Requirement | If you get it wrong |
|---|---|---|
| `ts_us` | **Absolute microseconds**, monotonically increasing. Any epoch will do (the peer only cares about deltas) | Timeline jumps / playback speed is nonsense |
| `payload` (video) | **AVCC**: `[4-byte big-endian NALU length][NALU]…`. **Not Annex-B** | Peer reports `Invalid NAL unit size` |
| `payload` (audio) | **Raw AAC**, with **no ADTS header** | Peer reports `Reserved bit set` / can't decode |
| `key` | Video **keyframes must pass 1**, other frames in the group 0; audio passes 0 | **The entire video track never appears** (below) |

**Don't put FLV tag headers or a container timestamp prefix in the payload** — that's
the library's business. It strips what you shouldn't have to care about and adds what
the peer needs. What you hand over is "the bare frame my encoder produced."

### Why `key` is a hard requirement

moq-lite has **no keyframe flag on the wire**. The receiver infers it from one
invariant: **the first frame of a group IS the keyframe.** The library opens a new
group on a keyframe and the rest of the group follows; **non-key frames that arrive
before a group is open are dropped** (a group with no keyframe is undecodable, so
sending it is pointless).

Consequences:

- if keyframes are 10 seconds apart, a peer joining inside those 10 seconds can only
  start at the next keyframe — that is inherent to moq-lite, not a bug;
- **but the library covers for you**: it holds the frames since the last keyframe
  (and the last ~1 second of audio), and a new subscriber starts from there — so a
  late viewer doesn't wait.

---

## 4. API reference

### Session

```c
moq_lite_session_t moq_lite_session_new(uv_loop_t *loop);
```
Creates a session, **without connecting**. Returns 0 on failure (null `loop` / OOM).
`loop` must outlive the session — don't `uv_loop_close` it before the session closes.

```c
int moq_lite_session_set_role(moq_lite_session_t s, moq_lite_role_t role);
```
Sets this end's role; **must be before `connect`**.

| Value | Meaning |
|---|---|
| `MOQ_LITE_ROLE_PUBLISHER` (1) | push only. **A push client wants this** |
| `MOQ_LITE_ROLE_SUBSCRIBER` (2) | pull only (the default) |
| `MOQ_LITE_ROLE_BOTH` (0) | both directions |

The value goes into SETUP's `Role` parameter, whose semantics are **"who the PEER
is"** — so a publisher sends 1 (the peer therefore acts as a subscriber and comes to
subscribe to us). **Forgetting it means the server treats you as a subscriber, never
sends a single ANNOUNCE_REQUEST, and nobody subscribes to what you push.**

```c
int moq_lite_session_connect(moq_lite_session_t s, const char *url,
                             const moq_lite_session_cb_t *cb);
```
Starts the connection (asynchronous); the result arrives via `cb->on_connected`.
Returns 0 if queued, <0 for an immediate failure.

- `url` looks like `moql://host:port/path`. Only `moql://` / `moq-lite://` schemes are
  accepted; `https://` returns `MOQ_LITE_ERR_URL` (that's the WebTransport path, which
  this library does not implement).
- **The path decides the server's routing**: the `anon` in `moql://host:4433/anon` is
  the server's entry path (`moqlivecast`'s `moq_lite_subpath`). Omitting it uses the
  server's default path.
- `host` accepts **both a domain name and an IP literal** — resolution goes through
  `uv_getaddrinfo`, and a numeric literal just returns immediately without touching
  the network. So `moql://relay.example.com:4433/anon` and
  `moql://192.168.1.221:4433/anon` both work.
  ⚠️ Resolution is **IPv4-only**: the QUIC send path here is hardcoded IPv4
  (`sockaddr_in` + `uv_ip4_addr`), so an IPv6-only domain won't connect (an AAAA
  answer would fail to parse and packets would go to `0.0.0.0`, silently).
  Certificates are a separate matter: TLS here is `SSL_VERIFY_NONE`, so a cert with
  no IP SAN still connects and an IP is fine. Clients that *do* verify (`moq-cli` /
  libmoq) need the domain instead.
- `cb` is **deep-copied**; you needn't keep it alive.

```c
const char *moq_lite_session_alpn(moq_lite_session_t s);
```
The negotiated ALPN, valid once the handshake completes. **Worth printing in
`on_connected`** — if it's wrong (say it landed on `h3`), the peer is treating you as
WebTransport.

```c
void moq_lite_session_close(moq_lite_session_t s);
```
Closes the session (safe from any thread). All handles become invalid afterwards; no
further free call is needed (it's idempotent). `on_close` is the **last** callback.

```c
int moq_lite_session_set_alpn(const char *const *protos, int count);
```
Overrides the offered ALPN list (process-global; **must be before the first connect**).
Without it you get the default `{MOQ_LITE_VERSION}` = `{"moq-lite-06"}`. **You normally
don't need this** — only when the peer recognizes a version you implemented that the
default list doesn't cover.

### Publishing

```c
moq_lite_broadcast_t moq_lite_publish(moq_lite_session_t s, const char *path);
```
Declares an outgoing broadcast; `path` is the on-wire broadcast name (e.g.
`"my_stream.hang"`). **Must be called after `connect` succeeds** — with no connection
there is nobody to announce to. Returns 0 on failure.

```c
moq_lite_track_t moq_lite_publish_track(moq_lite_broadcast_t bc, int is_video,
                                        const moq_lite_rendition_t *r);
```
Declares a track on the broadcast and **writes it into the catalog**. `is_video`
decides whether it lands in the catalog's `video` or `audio` section. Returns 0 on
failure.

**At least one track is required for the broadcast to be announced at all**: a
broadcast with no catalog is neither announced to the peer nor decodable (the server
simply does not serve a stream that has no catalog).

`moq_lite_rendition_t` fields by media type:

| Field | Video | Audio |
|---|---|---|
| `name` | Track name — **also the catalog `renditions` key** | same |
| `codec` | e.g. `"avc1.42c01e"`, must start with `avc1.` | `"mp4a.40.2"`, must start with `mp4a.40.` |
| `config` / `config_len` | **raw avcC bytes** | **raw AudioSpecificConfig bytes** |
| `width` / `height` | fill in | ignored |
| `sample_rate` / `channels` | ignored | fill in |
| `bitrate` | may be 0 | may be 0 |

> `config` holds **raw bytes** — not a hex string, not base64. (The catalog's
> `description` field is hex, but the library does that conversion for you.)
> From FLV: the avcC is the AVC sequence header with its 5-byte tag header stripped.

```c
int moq_lite_track_frame(moq_lite_track_t t, uint64_t ts_us,
                         const uint8_t *payload, size_t len, int key);
```
Writes one frame. `ts_us` is absolute microseconds; `key` is 1 for video keyframes.
Returns `MOQ_LITE_OK` or an error code. **Must be called on the loop thread.**

```c
void moq_lite_track_end(moq_lite_track_t t);
```
Finishes the track: writes the end-of-group sentinel plus FIN on every subscriber's
current group, and marks the track ended. Call it when you're done pushing — without
it, the peer's group stream never ends.

### Error codes

```c
#define MOQ_LITE_OK              0
#define MOQ_LITE_ERR_ARGS       -1   /* invalid handle / null argument */
#define MOQ_LITE_ERR_URL        -2   /* unparseable URL / not a raw-QUIC scheme */
#define MOQ_LITE_ERR_NOMEM      -3
#define MOQ_LITE_ERR_STATE      -4   /* session is not in a state that allows this */
#define MOQ_LITE_ERR_INTERNAL   -5

const char *moq_lite_strerror(int err);
```
`moq_lite_strerror` returns a **thread-local** human-readable description; safe to call
any time, no prior failure needed.

> ⚠️ **Functions that return a handle return `0` on failure, not a negative error
> code.** Handles are `uint32_t`; casting `-3` into one yields a huge non-zero value
> that `if (!h)` cannot detect. See *Troubleshooting*.

### Threading

| Call | Thread |
|---|---|
| `moq_lite_session_new` / `_set_role` / `_set_alpn` | any (before connect) |
| `moq_lite_session_connect` / `_close` | any thread |
| `moq_lite_publish` / `_publish_track` / `_track_frame` / `_track_end` | **loop thread** |
| all callbacks | loop thread |

### Lifetime

**Do not** free the session inside a callback — call `_close` after it returns. The
library guarantees objects are valid during a callback and that afterwards they may
already have been marked dead by the next action.

---

## 5. Building

The library is the CMake target `moqlite_client_core` (a static library with `PUBLIC`
uv/ssl/crypto dependencies):

```cmake
target_link_libraries(my_pusher PRIVATE moqlite_client_core)
```

`moqlite` is already in its `PUBLIC` include directories, so `#include
"moq_lite_client.h"` works directly. It's usable from both C and C++ (the header
carries `extern "C"`).

```bash
cmake -B build
cmake --build build --target moqlite_client_core
```

---

## 6. Troubleshooting

All of these fail **silently** (no error, no reset — just nothing further). Go by
symptom:

| Symptom | Most likely cause |
|---|---|
| Connected, but the **peer never reacts** | ① you forgot `set_role(PUBLISHER)` — the server treats you as a subscriber and won't subscribe to you; ② ALPN negotiated to `h3` (print `moq_lite_session_alpn()` to confirm) — the server takes you for WebTransport and waits for a CONNECT that never comes |
| **Video entirely missing, audio fine** | `key` is wrong: the keyframe didn't pass 1, so the library can't open a group — or the payload isn't AVCC (e.g. you passed Annex-B) |
| Peer reports `Invalid NAL unit size` | non-codec bytes in the video payload (FLV tag header / Annex-B start code) |
| Peer reports `Reserved bit set` / `Prediction is not allowed` | audio payload carries an ADTS header (it wants raw AAC), or audio/video packets got crossed |
| **The broadcast never shows up** on the peer | you declared no tracks — a broadcast with no catalog is not announced |
| `moq_lite_publish_track` says "invalid handle" | the broadcast handle came from **another session**, or `publish` actually failed while you treated it as success (check the return is 0) |
| Playback stalls midway | you never called `moq_lite_track_end()` |
| A later viewer gets **no picture** | expected, but only for as long as it takes to reach the next keyframe: the library holds the frames since the last keyframe. If it takes clearly longer, check that `key` is 1 on keyframes only |

Debugging advice: **first print `moq_lite_session_alpn()` and the return value of
every `publish*`, then log a "sent N frames" line.** Those three lines distinguish
most of the cases above.

---

## 7. The other direction (pulling)

The same library also plays the subscriber role (`on_announce` →
`moq_lite_subscribe` → `on_frame`); this document just focuses on pushing. For that
half:

- `moq_lite_session_announce()` / `moq_lite_subscribe()` / `moq_lite_unsubscribe()`
- `moq_lite_sub_cb_t`'s `on_frame` / `on_subscribe` / …
- `moq_lite_strip_container_ts()` — media frame payloads are prefixed with an
  **absolute-microsecond** varint; use this to strip it. **Data tracks like the
  catalog have no such prefix**, and stripping one anyway silently loses 2 bytes.

Every function's header comment documents these pitfalls in more detail than this
document does.

---

## 8. Further reading

| To learn about | See |
|---|---|
| The complete push implementation (FLV → moq-lite) | `tools/flv2moqlite_client/` |
| Wire encoding and byte-level self-tests | `moqlite/moq_lite_wire.h`, `moqlite/moq_lite_wire_test.c` |
