# moqtransport2flv_client

Subscribe to a `moqlivecast` stream through `moq_transport_client` and write
H.264 + AAC to an FLV file.  FLV muxing uses
`moqlivecast/format/flv/FlvMuxer`.

```bash
./build/bin/moqtransport2flv_client \
    <host> <port> <app> <stream> <out.flv> \
    [--duration seconds] [--path /moq]
```

Example:

```bash
# publisher
./build/bin/flv2moqtransport_client test.flv 127.0.0.1 4433 live demo

# subscriber
./build/bin/moqtransport2flv_client \
    127.0.0.1 4433 live demo /tmp/demo.flv --duration 10
```

- `host` may be an IPv4 literal or a domain with an IPv4 A record; the tool
  resolves it with `uv_getaddrinfo(AF_INET)` and keeps the domain as the
  WebTransport `:authority`. The QUIC stack itself is IPv4-only.
- Output is H.264 + AAC FLV; no other codecs.
- `--duration 0` (default) runs until Ctrl-C or the publisher closes the stream.
- The file contains the GOP available at subscribe time plus live objects.
- Timestamps are the server's pull timeline (the server may rebase a GOP to
  start near zero), not the publisher's absolute DTS.

Build:

```bash
cmake -B build
cmake --build build --target moqtransport2flv_client
```
