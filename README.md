# moqlivecast

**English** | [简体中文](README_CN.md)

A live media stack built on a from-scratch C implementation of QUIC, HTTP/3, and WebTransport. The server binary is `moqlivecast`. A Vue demo in `tools/moq_js_client` can publish and play from the browser.

## Features

- **MoQ Transport (draft-ietf-moq-transport-22) Push / Pull** over WebTransport
- **C SDK** — H.264 + AAC publish/subscribe with app/stream names, codec config,
  and per-object DTS/PTS
- **WebTransport FLV** Push / Pull
- **HTTP-FLV Pull** on the same port as WebTransport (UDP + TCP)
- **RTMP push / play** on TCP 1935 — OBS / ffmpeg can push, and any stream can
  be pulled as RTMP
- **Android push client** — Camera2 + mic → H.264/AAC → moq-lite-06 over raw QUIC
  (see the [Android guide](tools/android/how_to_use_android_moqlit_push.md))
- **Cluster (edge / origin)** — an edge pushes its local streams to a plain
  center node and pulls missing streams back on demand; every playback protocol
  on the edge is served from the pulled stream
- Shared `MediaStreamManager` with per-stream GOP cache, so a late subscriber can start from the last keyframe

## Architecture

```
   Ingest                                        Server                     Playback

   Browser                          ┌──────────────────────┐      Browser
   moq-transport ──────────────────>│     moqlivecast      │─────> moq-transport
                                    │          │           │
   ffmpeg ──FLV──> moq ────────────>│          v           │─────> moq ──> ffmpeg
   (moq-transport)                  │ MediaStreamManager   │      (moq-transport)
                                    │      + GOP cache     │
                                    └──────────────────────┘─────> HTTP-FLV pull
```

The browser path uses **h3 / WebTransport carrying moq-transport**; the
`ffmpeg + moq` path shares the same UDP port. That port also accepts raw QUIC
with ALPN `moq-lite-06` for Android, the cluster link, and `flv2moqlite_client`.
Both transports carry push and pull.

This repo speaks **two different MoQ dialects**. They share `MediaStreamManager`
and the GOP cache, but not the wire format:

- **moq-transport** — `draft-ietf-moq-transport-22`, over WebTransport/HTTP3
  (`h3` ALPN): browser `/moq` and `/moq-pull`, and `flv2moq_client`.
- **moq-lite-06** — over raw QUIC (`moq-lite-06` ALPN): Android, the
  edge↔center cluster link, `flv2moqlite_client`, and the `moqlite/` C API.

When this README says **MoQ push / pull** without qualification, it means
**moq-transport draft-22**. Anything `moq-lite-06` is marked explicitly.

The protocol stack (QUIC, HTTP/3, WebTransport) lives under `quic/` and `http3/`, built directly on libuv and OpenSSL — no third-party QUIC library is used. Live ingest, GOP cache, and per-protocol writers live under `moqlivecast/`.

### Cluster topology

The same binary also runs as a two-tier cluster: a **center** is an ordinary
server, and any number of **edges** dial it. An edge pushes each local stream
upstream (so other edges can find it) and pulls a stream back only when one of
its own players asks for it:

```text
   Edge 1                                      Center
   ┌──────────────────┐   push (moq-lite-06)    ┌──────────────────────┐
   │ local ingest     │ ───────────────────> │  MediaStreamManager  │
   │ + local players  │ <─────────────────── │  (plain server)      │
   └──────────────────┘   pull (on demand)   └──────────────────────┘
                                                        ^
                                               push / pull (moq-lite-06)
                                                        │
   Edge 2 ──────────────────────────────────────────────┘
```

The edge↔center link is **moq-lite-06 over raw QUIC**, one connection per stream
per direction. Setup and a localhost walkthrough: [Cluster
(edge / origin)](#cluster-edge--origin).

## Quick start

### Build

Requires CMake 3.10+, a C/C++14 toolchain, and (first build) time to compile the bundled OpenSSL.

```bash
cmake -B build
cmake --build build --target moqlivecast
```

The binary is `build/bin/moqlivecast`.

### Configure

`moqlivecast` takes a YAML file:

```bash
./build/bin/moqlivecast moqlivecast/etc/config.yml
```

Leave `certfile` / `keyfile` empty in the YAML and export the PEM paths instead. If both YAML fields are empty, `moqlivecast` reads `MOQ_SSL_CERT_FILE` and `MOQ_SSL_KEY_FILE`. If they are still empty, it prints the reason and exits. These names are deliberately `MOQ_`-prefixed so they do not collide with the standard `SSL_CERT_FILE` used by Python/OpenSSL clients. The legacy `SSL_CERT_FILE`/`SSL_KEY_FILE` variables are no longer read by `moqlivecast`.

```bash
export MOQ_SSL_CERT_FILE=/path/to/fullchain.pem
export MOQ_SSL_KEY_FILE=/path/to/privkey.pem
./build/bin/moqlivecast moqlivecast/etc/config.yml
```

WebTransport needs a certificate whose hostname matches the URL you open in the browser. A raw `https://127.0.0.1:…` origin usually fails in Chrome; use a hosts entry and keep that host out of any HTTP proxy.

Example shape:

```yaml
moq_server:
  listenip: 0.0.0.0
  port: 4433
  subpath:
    - desc: media format is flv
      path: flv
    - desc: media format is moq
      path: moq
  certfile: ""
  keyfile: ""

httpflv:
  listenip: 0.0.0.0
  port: 4433

log:
  level: info
  filename: /tmp/moqlivecast.log
  console: false
```

### Browser demo

```bash
cd tools/moq_js_client
npm install
npm run dev
```

Vite listens on `http://127.0.0.1:5174`.

| Path | Role |
|---|---|
| `/moq` | MoQ Transport (draft-22) Push |
| `/moq-pull` | MoQ Transport (draft-22) Pull |
| `/flv` | WebTransport FLV Push |
| `/pull` | WebTransport FLV Pull |
| `/http-flv` | HTTP-FLV Pull |

After changing C++ code, rebuild and restart `moqlivecast`. The browser demo does not pick up a new server by itself.

### Android push demo

`tools/android/` is a complete push client: Camera2 + microphone → MediaCodec
H.264/AAC → the `moqlite/` C API → raw QUIC (`moq-lite-06`) → `moqlivecast`.
It is also the reference for the Android JNI/lifecycle plumbing.

```bash
bash tools/android/build_openssl_android.sh        # once per clean tree
cd tools/android && ./gradlew :app:assembleDebug
adb install -r app/build/outputs/apk/debug/app-debug.apk
```

In the app, enter the server's **LAN IP** (e.g. `moql://192.168.1.143:4433/anon`)
and a broadcast name (default `android.hang`), grant camera + microphone, and
press **Start**. Pull it in the browser demo at `/moq-pull` with:

```
https://<server-host>:4433/moq?app=anon&stream=android.hang
```

Full build options, source map, invariants and troubleshooting:
**[tools/android/how_to_use_android_moqlit_push.md](tools/android/how_to_use_android_moqlit_push.md)**.

### RTMP push

RTMP ingest listens on its own **TCP** port (`rtmp:` in
[`moqlivecast/etc/config.yml`](moqlivecast/etc/config.yml), 1935 by default)
and feeds the same `MediaStreamManager`, so every egress below can play it.

```bash
# OBS: Server = rtmp://<host>:1935/live, Stream Key = abc
ffmpeg -re -i test.flv -c copy -f flv rtmp://<host>:1935/live/abc
```

Pull the result like any other stream:

```
https://<server-host>:4433/moq?app=live&stream=abc   # browser /moq-pull
https://<server-host>:4433/live/abc.flv              # HTTP-FLV
```

The RTMP server is also an egress: any stream in `MediaStreamManager` (whatever
its source) can be pulled as RTMP, and it starts from the cached GOP:

```bash
ffplay rtmp://<host>:1935/live/abc
```

RTMP → **moq-lite-06** (raw QUIC, `moq-cli` / the Android client) is supported for
**H.264 + AAC**: the server synthesizes `catalog.json` from the FLV sequence
headers. The RTMP **app must match the moq-lite-06 entry path** (default `anon`),
because the export resolves `<entry>/<broadcast>`:

```bash
# push with app=anon, then pull it as moq-lite-06 (not moq-transport)
ffmpeg -re -i test.flv -c copy -f flv rtmp://<host>:1935/anon/123456
moq --connect "moql://<host>:4433/anon" --broadcast "123456" export flv | ffplay -
```

Other codecs are passed through to HTTP-FLV / RTMP but are not exported to
moq-lite-06.

### Cluster (edge / origin)

Two roles, one binary:

| Role | Config | What it does |
|---|---|---|
| **Center** | none — leave `cluster.mode` off | An ordinary server. Receives edge pushes and serves edge pulls. |
| **Edge** | `cluster.mode: edge` + `center_address` | Pushes every local stream to the center; pulls a stream from the center when a local player asks for one it does not have. |

```yaml
cluster:
  mode: edge                     # off (default) = standalone; the center stays off
  protocol: moq                  # phase 1: moq-lite-06 over raw QUIC only
  center_address: 10.0.0.1:4433  # center's moq-lite-06 UDP port, not RTMP/HTTP-FLV
  center_path: anon              # center's moq_lite_subpath entry
```

What an edge does:

- **Push** — when a local stream gets a catalog, the edge opens one `moq-lite-06`
  connection to the center and publishes the catalog and every track. A local
  unpublish pauses the push for 30 s (so an encoder restart does not tear the
  link down) before closing it.
- **Pull** — when a local player asks for a stream the edge does not have, the
  edge opens one `moq-lite-06` connection, subscribes to the catalog and tracks,
  and writes the frames into its own `MediaStreamManager`. Playback then takes
  the normal local path, so **any** egress on the edge works: moq-lite-06,
  WebTransport, HTTP-FLV, RTMP.
- **Discovery** — a read-only connection subscribes to the center's announce
  stream, so local clients can see remote broadcasts. Without it they would never
  subscribe, and nothing would trigger a pull.
- **Loop prevention** — a pulled stream is never pushed back upstream, and a
  local publisher always wins over a pull (the pull is abandoned).

Only streams with a moq-lite-06 catalog travel over the link — today that means
H.264 + AAC, the same rule as the RTMP → moq-lite-06 export.

Run all three nodes on localhost with the shipped configs
([`config.yml`](moqlivecast/etc/config.yml), [`edge1.yml`](moqlivecast/etc/edge1.yml),
[`edge2.yml`](moqlivecast/etc/edge2.yml)):

```bash
export MOQ_SSL_CERT_FILE=/path/to/fullchain.pem
export MOQ_SSL_KEY_FILE=/path/to/privkey.pem

./build/bin/moqlivecast moqlivecast/etc/config.yml  # center: UDP 4433, RTMP 1935, HTTP-FLV 4433
./build/bin/moqlivecast moqlivecast/etc/edge1.yml   # edge 1: UDP 4434, RTMP 1936, HTTP-FLV 8081
./build/bin/moqlivecast moqlivecast/etc/edge2.yml   # edge 2: UDP 4435, RTMP 1937, HTTP-FLV 8082
```

Push into edge 1, play from edge 2 (which pulls the stream from the center):

```bash
ffmpeg -re -i test.flv -c copy -f flv rtmp://127.0.0.1:1936/anon/123456
ffplay rtmp://127.0.0.1:1937/anon/123456               # edge 2 pulls on demand
ffplay https://127.0.0.1:8082/anon/123456.flv          # same stream, HTTP-FLV
moq --connect "moql://127.0.0.1:4435/anon" --broadcast 123456 export flv | ffplay -  # moq-lite-06 pull
```

The RTMP `app` must match the moq-lite-06 entry path (`anon`), as in the RTMP
section above: the key on the wire is `<app>/<stream>`.

Knobs, all under `cluster:` in
[`config.yml`](moqlivecast/etc/config.yml):

| Key | Default | Meaning |
|---|---|---|
| `reconnect_min_ms` / `reconnect_max_ms` | 500 / 10000 | Outbound reconnect backoff range (exponential, ±20 % jitter) |
| `subscribe_timeout_ms` | 3000 | How long a local moq-lite-06 SUBSCRIBE waits for a pull to produce a catalog |
| `pull_acquire_timeout_ms` | 5000 | Give up a pull if no catalog arrives within this budget (across reconnects) |
| `pull_idle_timeout_ms` | 30000 | Close a pull if no media frame arrives for this long |
| `max_pull_connections` | 128 | Concurrent pulls per edge |
| `negative_cache_ms` | 5000 | Do not retry a stream that just failed to acquire |

The center needs no cluster config at all; it is a plain server that happens to
receive edge connections.

## Wire protocol: moq-transport (draft-ietf-moq-transport-22)

This section is about **moq-transport**, not `moqlite/`. `moqlite/` implements
the separate moq-lite-06 wire format over raw QUIC.

We follow the stream taxonomy of **[draft-ietf-moq-transport](https://datatracker.ietf.org/doc/draft-ietf-moq-transport/)
§3.3** (the numbering below is from the draft currently tracked here —
control-flow separation landed on top of the layout described there).

The spec splits messages into three categories, and **they do not all use
the same stream type**:

| Category | Messages | Stream type | Status here |
|---|---|---|---|
| **Control** | `SETUP` (`0x2f00`), `GOAWAY` (`0x10`) | **Unidirectional** — each peer opens one; the pair carries both directions | ✅ implemented |
| **Request** | `PUBLISH` (`0x1d`), `SUBSCRIBE` (`0x3`) | **Bidirectional** — one stream per request | ✅ implemented |
| **Response** | `SUBSCRIBE_OK` (`0x4`), `PUBLISH_DONE` (`0xb`) | **Same request stream, reverse direction** — *not* a separate stream | ✅ implemented |
| **Object** | `SUBGROUP` + `OBJECT` | **Unidirectional** | ✅ implemented — publisher opens them when publishing, server opens them when serving subscribers |

Two points are easy to get wrong and are worth calling out, because we got
them wrong ourselves first:

1. **Responses ride the request stream back, not the control stream.**
   §3.3.2: *"A request stream is bidirectional and each direction is closed
   independently."* So `SUBSCRIBE_OK` travels in the reverse direction of the
   `SUBSCRIBE` request stream. The message-type table in the draft marks it
   `Request` (not `Control`), which is the authoritative signal.
   A response is **not** sent as a newly opened unidirectional stream.

2. **Objects can only go on unidirectional streams** (*"Objects are sent on
   unidirectional streams"*), which is independent of the control/request
   split. A unidirectional stream has **no "write the alias back" step**: the
   receiver learns which track the objects belong to from the Track Alias in
   the `SUBGROUP` header, and the order streams are opened carries no meaning.
   So the sender has to open the stream itself — the publisher opens them when
   publishing, and the server opens them when serving a subscriber (the client
   no longer opens data streams at all).

### What this means in practice

`flv2moq_client` opens **7 streams** for a push: 1 control (uni, `SETUP`) +
3 request (bidi, one `PUBLISH` each for catalog/video/audio) + 3 data streams
(uni). A `moq` pull has the client open only **2 streams**: 1 control
(uni, `SETUP`) + 1 request (bidi, carrying both `SUBSCRIBE`s and reading their
`SUBSCRIBE_OK`s back); the data streams are opened by the **server**, one
unidirectional stream per track.

### Discussing this with others

If you are comparing implementations: the shape to check first is whether a
peer puts `PUBLISH`/`SUBSCRIBE` on a bidirectional stream (correct) or on the
control unidirectional stream alongside `SETUP` (a common simplification).
The second shape works as long as both ends agree, but it is not what the
draft specifies and it will not interoperate.

## Repository layout

| Path | What it is |
|---|---|
| `quic/` `http3/` | C protocol stack (QUIC, HTTP/3, WebTransport), built on libuv + OpenSSL |
| `moqlivecast/` | C++ live server |
| `moqlivecast/cluster/` | Edge-side cluster: discovery, upstream push, on-demand pull |
| `wt2ws/` | WebTransport ↔ WebSocket relay for the last mile — **[guide](wt2ws/README.md)** (Vue test client + Python echo backend) |
| `wt2ws/StreamVox/` | WebSocket voice agent (OpenAI Realtime-compatible) — **[guide](wt2ws/StreamVox/README.md)** |
| `moq_transport_client/` | C SDK for **moq-transport draft-22** (H.264 + AAC publish/subscribe over WebTransport) — **[guide](moq_transport_client/README.md)** |
| `moqlite/` | **separate** moq-lite-06 C client library (bare QUIC, not moq-transport) — **[guide: writing a push client](moqlite/how_to_use_moq_lite_c_api.md)** · [中文](moqlite/how_to_use_moq_lite_c_api_CN.md) |
| `tools/moq_js_client/` | Browser publish / play demo (moq-transport over WebTransport) |
| `tools/android/` | Android push client (Camera2 + mic → moq-lite-06) — **[guide](tools/android/how_to_use_android_moqlit_push.md)** |
| `tools/moqtransport2flv_client/` | **moq-transport draft-22** pull → H.264/AAC FLV file, using the C SDK |
| `tools/flv2moqtransport_client/` | FLV file → **moq-transport draft-22** publish using the C SDK |
| `tools/flv2moq_client/` | FLV file → **moq-transport (draft-22) over WebTransport** push (C++) |
| `tools/flv2moqlite_client/` | FLV file → **moq-lite-06** push (C++, uses the `moqlite/` C API) |
| `http3/wt_chat_js_client/` | WebTransport signaling demo (with `wt_chat_server`) |
| `udp/` `tls/` | Standalone UDP / TLS echo samples (not on the server's main path) |
| `3rdparty/` | libuv, OpenSSL, yaml-cpp |

## Status

This is an early public snapshot, not a complete IETF MoQ implementation.

Working today: publish and play over **moq-transport (draft-22)**, WebTransport
FLV, and HTTP-FLV, with GOP catch-up for new subscribers. **RTMP push / play** (OBS / ffmpeg) works on
TCP 1935 in both directions, and H.264 + AAC RTMP streams are also exported to
**moq-lite-06** (raw QUIC) via a synthesized catalog.

**Cluster** (two-tier edge / origin) works: an edge pushes its local streams to a
plain center, announces the center's broadcasts to its local clients, and pulls a
missing stream on demand — playback on the edge works over moq-lite-06,
moq-transport over WebTransport, HTTP-FLV and RTMP. See [Cluster (edge / origin)](#cluster-edge--origin).

Not finished: MoQ catalog / control-plane completeness (`GOAWAY` and other
control messages are parsed but not acted on); catalog updates for
already-connected moq-lite-06 subscribers; RTMPS and push authentication.

## Roadmap

**Cluster hardening** — the two-tier path works end to end, but the link still
has room to grow:

- TLS on the edge↔center link is `SSL_VERIFY_NONE` (like every other client in
  this repo) and the link has no authentication; don't expose it to an untrusted
  network yet.
- One QUIC connection per stream per direction. Multiplexing many streams onto
  one connection would cut handshake and NAT-state overhead on large edges.
- Only the moq-lite-06 link protocol is implemented (`cluster.protocol: rtmp` is
  rejected), and there is no multi-level or mesh topology yet.

After that: catalog and control-plane completeness, live catalog updates for
connected moq-lite-06 subscribers, RTMPS and push authentication.

## License

MIT. See [LICENSE](LICENSE).
