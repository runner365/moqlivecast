from __future__ import annotations

from ..config import Settings
from .base import TtsClient
from .mock import MockTts


def create_tts(settings: Settings) -> TtsClient:
    provider = settings.tts_provider
    if provider == "mock":
        return MockTts(sample_rate=settings.tts_sample_rate)
    if provider == "bailian":
        from .bailian import BailianTts

        if not settings.dashscope_api_key:
            raise RuntimeError(
                "TTS_PROVIDER=bailian requires DASHSCOPE_API_KEY "
                "(or set TTS_PROVIDER=mock)"
            )
        return BailianTts(
            api_key=settings.dashscope_api_key,
            model=settings.tts_model,
            voice=settings.tts_voice,
            sample_rate=settings.tts_sample_rate,
            speech_rate=settings.tts_speech_rate,
            ws_url=settings.dashscope_ws_url or None,
        )
    raise RuntimeError(f"unknown TTS_PROVIDER: {provider}")


__all__ = ["TtsClient", "MockTts", "create_tts"]
