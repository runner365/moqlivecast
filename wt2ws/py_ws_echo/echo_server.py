#!/usr/bin/env python3
"""Minimal WebSocket echo server used to bring up / test wt2ws_proxy.

It is deliberately dumb: every text or binary message is sent back verbatim
(after an optional artificial delay).  That means the proxy can stay a pure
relay -- it never has to understand the payload -- and a client can measure
round-trip time simply by echoing its own seq/timestamp fields.

Usage:
    python3 echo_server.py --host 127.0.0.1 --port 9001
    python3 echo_server.py --port 9001 --delay-ms 200      # fake ASR/TTS think time
"""

import argparse
import asyncio
import json
import logging
import time

try:  # websockets >= 12
    from websockets.asyncio.server import serve
except ImportError:  # websockets 11.x and older
    from websockets.server import serve  # type: ignore

LOG = logging.getLogger("py_ws_echo")


def _path(ws) -> str:
    """Return the request path across old/new websockets versions."""
    req = getattr(ws, "request", None)
    if req is not None:
        return getattr(req, "path", "/")
    return getattr(ws, "path", "/")


def _peer(ws) -> str:
    addr = getattr(ws, "remote_address", None)
    if not addr:
        return "?"
    return f"{addr[0]}:{addr[1]}"


def _summary(msg) -> str:
    if isinstance(msg, (bytes, bytearray)):
        return f"{len(msg)}B binary"
    try:
        obj = json.loads(msg)
    except Exception:
        return f"{len(msg)}B text"
    if isinstance(obj, dict):
        seq = obj.get("seq")
        typ = obj.get("type")
        if seq is not None or typ is not None:
            return f"{len(msg)}B text type={typ} seq={seq}"
    return f"{len(msg)}B text"


async def handler(ws):
    started = time.monotonic()
    count = 0
    LOG.info("open   %s path=%s", _peer(ws), _path(ws))
    try:
        async for msg in ws:
            count += 1
            if args.delay_ms:
                await asyncio.sleep(args.delay_ms / 1000.0)
            await ws.send(msg)
            LOG.info("echo#%-4d %s", count, _summary(msg))
    except Exception as exc:  # ConnectionClosed and friends
        LOG.info("close  %s after %d msg(s), %.1fs: %s",
                 _peer(ws), count, time.monotonic() - started, exc)
        return
    LOG.info("close  %s after %d msg(s), %.1fs",
             _peer(ws), count, time.monotonic() - started)


async def main():
    logging.basicConfig(
        level=getattr(logging, args.log_level.upper(), logging.INFO),
        format="%(asctime)s %(levelname)-5s %(message)s",
        datefmt="%H:%M:%S",
    )
    LOG.info("listening ws://%s:%d  delay=%dms max_size=%d",
             args.host, args.port, args.delay_ms, args.max_size)
    async with serve(handler, args.host, args.port,
                     max_size=args.max_size,
                     ping_interval=20, ping_timeout=20):
        await asyncio.Future()  # run forever


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=9001)
    parser.add_argument("--delay-ms", type=int, default=0,
                        help="artificial think time before echoing (ms)")
    parser.add_argument("--max-size", type=int, default=8 * 1024 * 1024,
                        help="max WebSocket message size in bytes")
    parser.add_argument("--log-level", default="info")
    args = parser.parse_args()
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
