# moqlivecast

**English** | [简体中文](README_CN.md)

A live media stack built on a from-scratch C implementation of QUIC, HTTP/3, and WebTransport. The server binary is `moqlivecast`. A Vue demo in `tools/moq_js_client` can publish and play from the browser.

## Features

- **MoQ Push / Pull** over WebTransport
- **WebTransport FLV** Push / Pull
- **HTTP-FLV Pull** on the same port as WebTransport (UDP + TCP)
- Shared `MediaStreamManager` with per-stream GOP cache, so a late subscriber can start from the last keyframe

## Architecture

```
Browser  --WebTransport / HTTPS-->  moqlivecast
                                      |
                                      v
                              MediaStreamManager
                                      |
                         +------------+------------+
                         |            |            |
                      MoQ Push    WT FLV      HTTP-FLV
                      MoQ Pull    WT FLV Pull    Pull
```

The protocol stack (QUIC, HTTP/3, WebTransport) lives under `quic/` and `http3/`, built directly on libuv and OpenSSL — no third-party QUIC library is used. Live ingest, GOP cache, and per-protocol writers live under `moqlivecast/`.

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

Leave `certfile` / `keyfile` empty in the YAML and export the PEM paths instead. If both YAML fields are empty, `moqlivecast` reads `SSL_CERT_FILE` and `SSL_KEY_FILE`. If they are still empty, it prints the reason and exits.

```bash
export SSL_CERT_FILE=/path/to/fullchain.pem
export SSL_KEY_FILE=/path/to/privkey.pem
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
| `/moq` | MoQ Push |
| `/moq-pull` | MoQ Pull |
| `/flv` | WebTransport FLV Push |
| `/pull` | WebTransport FLV Pull |
| `/http-flv` | HTTP-FLV Pull |

After changing C++ code, rebuild and restart `moqlivecast`. The browser demo does not pick up a new server by itself.

## Wire protocol: how our streams map to the spec

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
| `tools/moq_js_client/` | Browser publish / play demo |
| `http3/wt_chat_js_client/` | WebTransport signaling demo (with `wt_chat_server`) |
| `udp/` `tls/` | Standalone UDP / TLS echo samples (not on the server's main path) |
| `3rdparty/` | libuv, OpenSSL, yaml-cpp |

## Status

This is an early public snapshot, not a complete IETF MoQ implementation.

Working today: publish and play over MoQ, WebTransport FLV, and HTTP-FLV, with GOP catch-up for new subscribers.

Not finished: MoQ catalog / control-plane completeness (`GOAWAY` and other
control messages are parsed but not acted on), and starting the existing RTMP
code path from `moqlivecast`.

## Roadmap

**Cluster / origin pull** — the next major piece. Goal: run `moqlivecast` as
a cluster, where edge nodes pull streams from an origin node on demand,
so one origin can feed many edges (the standard live-CDN topology).

The building blocks are already in the repo: a QUIC client API
(`quic/quic_client_api.c`) and a WebTransport client API
(`http3/webtransport_client_api.c`). What's missing is the origin-side
pull-and-republish logic.

## License

MIT. See [LICENSE](LICENSE).
