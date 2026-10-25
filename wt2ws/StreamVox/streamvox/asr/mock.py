from __future__ import annotations

import asyncio

from .base import AsrClient, AsrResult, AsrStream


class MockAsrStream(AsrStream):
    def __init__(self, text: str, delay_ms: int = 150):
        self._text = text
        self._delay = max(0.0, delay_ms / 1000.0)
        self._pcm = bytearray()
        self._cancelled = False

    async def feed(self, pcm16: bytes) -> None:
        if not self._cancelled:
            self._pcm.extend(pcm16)

    async def finish(self) -> AsrResult:
        if self._cancelled:
            return AsrResult(text="", raw={"provider": "mock", "cancelled": True})
        if self._delay:
            await asyncio.sleep(self._delay)
        text = self._text.strip() or f"[mock asr] received {len(self._pcm)} bytes"
        return AsrResult(text=text, raw={"provider": "mock", "streaming": True,
                                        "bytes": len(self._pcm)})

    async def cancel(self) -> None:
        self._cancelled = True
        self._pcm.clear()


class MockAsr(AsrClient):
    """Deterministic ASR for local development and tests."""

    name = "mock"
    supports_streaming = True

    def __init__(self, text: str, delay_ms: int = 150):
        self._text = text
        self._delay = max(0.0, delay_ms / 1000.0)

    async def open_stream(self, sample_rate: int = 16000) -> AsrStream:
        return MockAsrStream(self._text, int(self._delay * 1000))

    async def transcribe(self, pcm16: bytes, sample_rate: int = 16000) -> AsrResult:
        stream = MockAsrStream(self._text, int(self._delay * 1000))
        await stream.feed(pcm16)
        return await stream.finish()
