from __future__ import annotations

from abc import ABC, abstractmethod
from collections.abc import AsyncIterator


class TtsClient(ABC):
    """Text to speech.  Output is raw mono PCM16, little endian."""

    name: str = "base"
    sample_rate: int = 24000

    @abstractmethod
    async def synthesize(self, text: str) -> AsyncIterator[bytes]:
        raise NotImplementedError

    @staticmethod
    def pcm_chunks(pcm: bytes, sample_rate: int, chunk_ms: int = 20) -> list[bytes]:
        step = max(2, int(sample_rate * chunk_ms / 1000) * 2)
        return [pcm[i:i + step] for i in range(0, len(pcm), step)]
