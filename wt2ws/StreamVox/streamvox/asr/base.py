from __future__ import annotations

from abc import ABC, abstractmethod
from dataclasses import dataclass, field
from typing import Any


@dataclass
class AsrResult:
    text: str
    raw: dict[str, Any] = field(default_factory=dict)


class AsrStream(ABC):
    """An in-progress recognition session.

    Audio is fed while the user is still speaking; `finish()` is called on
    commit and returns only the final transcript.  Partial results are not
    surfaced.
    """

    @abstractmethod
    async def feed(self, pcm16: bytes) -> None:
        raise NotImplementedError

    @abstractmethod
    async def finish(self) -> AsrResult:
        raise NotImplementedError

    @abstractmethod
    async def cancel(self) -> None:
        raise NotImplementedError


class AsrClient(ABC):
    """Speech to text.  Input is raw mono PCM16, little endian."""

    name: str = "base"
    supports_streaming: bool = False

    async def open_stream(self, sample_rate: int = 16000) -> AsrStream | None:
        """Return a streaming session, or None to use transcribe() instead."""
        return None

    @abstractmethod
    async def transcribe(self, pcm16: bytes, sample_rate: int = 16000) -> AsrResult:
        raise NotImplementedError
