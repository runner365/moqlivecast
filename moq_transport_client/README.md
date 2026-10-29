# moq_transport_client

C SDK for **moq-transport (draft-ietf-moq-transport-22) over WebTransport**,
focused on live H.264 + AAC publish / subscribe.

One client instance represents one broadcast:

```text
client -> connect(host, port, path="/moq", app, stream, direction, timeout_ms)
```

`app` + `stream` are the MoQ namespace; they are sent in `PUBLISH` / `SUBSCRIBE`.

## API shape

```c
moqt_client_t *c = moqt_client_new(loop, &callbacks, user);

/* Publisher: add video + audio before connect. */
moqt_client_add_stream(c, &(moqt_stream_config_t){
    .codec = MOQT_CODEC_H264,
    .config = avcC, .config_len = avcC_len,
    .codec_name = "avc1.64001f",
});
moqt_client_add_stream(c, &(moqt_stream_config_t){
    .codec = MOQT_CODEC_AAC,
    .config = asc, .config_len = asc_len,
    .codec_name = "mp4a.40.2",
});
moqt_client_connect(c, host, port, "/moq", app, stream,
                    MOQT_DIRECTION_PUBLISH, 5000);

/* If you resolve DNS yourself (recommended for domains), keep the original
 * host for :authority and pass the resolved IPv4 separately: */
moqt_client_connect_ex(c, "example.com", "1.2.3.4", 4433, "/moq",
                       app, stream, MOQT_DIRECTION_PUBLISH, 5000);

/* In on_stream_ready(video/audio): */
moqt_stream_write_ex(stream, data, len, dts_ms, pts_ms, keyframe,
                     on_write_done, user);

/* Subscriber: */
moqt_client_connect(c, host, port, "/moq", app, stream,
                    MOQT_DIRECTION_SUBSCRIBE, 5000);
/* on_data(stream, data, len, dts_ms, pts_ms, keyframe) is called per object. */
```

- H.264 payload is **AVCC** (length-prefixed NAL units), AAC is a raw frame.
- `dts_ms` / `pts_ms` are milliseconds. `pts - dts` is sent as
  `LOC_COMPOSITION_TIME` when non-zero.
- `avcC` / AAC `AudioSpecificConfig` are attached to the first object as
  `LOC_VIDEO_CONFIG` / `LOC_AUDIO_CONFIG`.
- `on_connected(client, status, reason, user)` fires exactly once:
  `MOQT_OK` means the WebTransport session is ready; otherwise `reason`
  explains the failure.  A failed connect does not also fire `on_closed`.
- `on_stream_ready` for a publisher fires after the PUBLISH and data stream
  are ready.  Do not call writes before that.
- For a subscriber, `on_stream_ready` fires when the first object of a track
  is parsed; `config` may be NULL if that object carried no config.

## Current limits

- The SDK itself does not perform DNS; use `moqt_client_connect_ex()` with
  `uv_getaddrinfo(AF_INET)` for domains.  The QUIC stack is IPv4-only.
- One broadcast and one direction per client; no reconnection logic.
- No internal write queue and no automatic frame dropping.  `moqt_stream_write`
  returns `MOQT_OK` when queued, not when ACKed.  Use `_ex` for completion
  callbacks and `inflight_bytes` / `high_watermark` for pacing.
- `on_write_done` reports `bytes == len` on success (all ACKed) and `0` on
  failure; QUIC stream writes are all-or-nothing.
- `high_watermark` is in **payload bytes**, not wire bytes.  When
  `inflight + len > watermark`, writes return `MOQT_ERR_AGAIN`; the caller
  decides whether to drop non-key video.
- Publisher requires both video and audio.  A video-only variant can be
  added by relaxing the check in `moqt_client_connect`.
- The server may rebase media timestamps when replaying a GOP (a pull starts
  near 0); subscribers should treat timestamps as a continuous timeline,
  not as the publisher's absolute DTS.
- Only draft-22 LOC; no moq-lite compatibility.

## Build

```bash
cmake -B build
cmake --build build --target moq_transport_client moq_transport_client_smoke
```

## Smoke test

Start `moqlivecast` with its usual `/moq` endpoint, then:

```bash
./build/bin/moq_transport_client_smoke 127.0.0.1 4433 live sdk_test
```

The test publishes 10 video/audio objects and subscribes to the same stream.
It seeds a keyframe before the subscriber connects, checks write-completion
callbacks, verifies `inflight_bytes` returns to 0, and exercises the
high-watermark `MOQT_ERR_AGAIN` path.
