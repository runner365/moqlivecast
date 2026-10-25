from __future__ import annotations

import asyncio
import math
from collections.abc import AsyncIterator

from .base import TtsClient


class MockTts(TtsClient):
    """Generates a short 440 Hz tone whose length follows the text length.

    Enough to exercise the browser's PCM playback path without any cloud call.
    """

    name = "mock"

    def __init__(self, sample_rate: int = 24000):
        self.sample_rate = sample_rate

    async def synthesize(self, text: str) -> AsyncIterator[bytes]:
        duration = min(5.0, max(0.5, len(text) * 0.07))
        total = int(duration * self.sample_rate)
        pcm = bytearray(total * 2)
        amplitude = 0.12 * 32767
        for i in range(total):
            value = int(amplitude * math.sin(2 * math.pi * 440 * i / self.sample_rate))
            pcm[2 * i] = value & 0xff
            pcm[2 * i + 1] = (value >> 8) & 0xff
        for chunk in self.pcm_chunks(bytes(pcm), self.sample_rate):
            yield chunk
            await asyncio.sleep(0.005)
