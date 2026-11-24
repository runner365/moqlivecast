# flv2moqtransport_client

Read an FLV file and publish it with `moq_transport_client` (H.264/H.265 +
AAC), paced by the file's DTS.  Video-only, audio-only, and video+audio FLV
inputs are supported.

```bash
./build/bin/flv2moqtransport_client <input.flv> <host> <port> <app> <stream> \
    [--speed 1.0] [--stats 5] [--path /moq] [--console 1|true]
```

Example:

```bash
./build/bin/flv2moqtransport_client test.flv 127.0.0.1 4433 live demo
```

- `host` may be an IPv4 literal or a domain with an IPv4 A record; the tool
  resolves it with `uv_getaddrinfo(AF_INET)` and keeps the domain as the
  WebTransport `:authority`. The QUIC stack itself is IPv4-only.
- The FLV may contain H.264 (`avcC`) or H.265/HEVC (`hvcC`) video, AAC audio
  (`AudioSpecificConfig`), or only one of them.  H.265 is sent with
  `MOQT_CODEC_H265` and the hvcC record as the MoQ video config.
- Frames are sent in file DTS order; `--speed 2` sends twice as fast.
- Logs are written to `/tmp/flv2moqtransport_client.log` by default.
  Add `--console 1` (or `--console true`) to also print them to the console.
- Set `MOQ_CLIENT_DEBUG=1` to include per-video-frame `dts` / wall-clock
  diagnostics (`[client-debug]`).
- The tool prints sent/ACK counts and exits non-zero if any write fails.

This is a demo client for the SDK; it keeps the whole pending frame queue in
memory. For very large files, use `--speed` or add file-side streaming.
