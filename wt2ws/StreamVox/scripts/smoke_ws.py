#!/usr/bin/env python3
"""Send one silent utterance through a running StreamVox and print events.

    python scripts/smoke_ws.py [ws://127.0.0.1:8000/ws]
"""
from __future__ import annotations

import asyncio
import base64
import json
import sys
import time

import websockets


async def main(url: str) -> int:
    chunk = b"\x00\x00" * 1600  # 100 ms @ 16 kHz mono PCM16
    async with websockets.connect(url, max_size=32 * 1024 * 1024) as ws:
        print("connected:", await ws.recv())
        await ws.send(json.dumps({
            "type": "session.update",
            "session": {"instructions": "smoke test", "voice": "test"},
        }))
        # Simulate push-to-talk: stream 1 s of audio in 10 chunks before commit.
        for _ in range(10):
            await ws.send(json.dumps({
                "type": "input_audio_buffer.append",
                "audio": base64.b64encode(chunk).decode(),
            }))
            await asyncio.sleep(0.05)
        t_commit = time.monotonic()
        await ws.send(json.dumps({"type": "input_audio_buffer.commit"}))
        await ws.send(json.dumps({"type": "response.create"}))

        audio_bytes = 0
        for _ in range(500):
            raw = await asyncio.wait_for(ws.recv(), timeout=30)
            event = json.loads(raw)
            kind = event.get("type", "?")
            if kind.endswith("audio.delta"):
                audio_bytes += len(base64.b64decode(event.get("delta", "")))
            if kind == "conversation.item.input_audio_transcription.completed":
                print("transcript: %r  (%.2fs after commit)" % (
                    event.get("transcript"), time.monotonic() - t_commit))
            if kind == "response.done":
                print("done:", event["response"]["status"],
                      "audio_bytes=", audio_bytes)
                return 0 if audio_bytes > 0 else 1
        print("no response.done")
        return 1


if __name__ == "__main__":
    url = sys.argv[1] if len(sys.argv) > 1 else "ws://127.0.0.1:8000/ws"
    raise SystemExit(asyncio.run(main(url)))
