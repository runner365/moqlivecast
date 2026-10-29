# flv2moqtransport_client

Read an FLV file and publish it with `moq_transport_client` (H.264 + AAC),
paced by the file's DTS.

```bash
./build/bin/flv2moqtransport_client <input.flv> <host> <port> <app> <stream> \
    [--speed 1.0] [--stats 5] [--path /moq]
```

Example:

```bash
./build/bin/flv2moqtransport_client test.flv 127.0.0.1 4433 live demo
```

- `host` may be an IPv4 literal or a domain with an IPv4 A record; the tool
  resolves it with `uv_getaddrinfo(AF_INET)` and keeps the domain as the
  WebTransport `:authority`. The QUIC stack itself is IPv4-only.
- The FLV must contain H.264 video (`avcC`) and AAC audio (`AudioSpecificConfig`).
- Frames are sent in file DTS order; `--speed 2` sends twice as fast.
- The tool prints sent/ACK counts and exits non-zero if any write fails.

This is a demo client for the SDK; it keeps the whole pending frame queue in
memory. For very large files, use `--speed` or add file-side streaming.
