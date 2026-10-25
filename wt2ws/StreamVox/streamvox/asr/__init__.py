from __future__ import annotations

from ..config import Settings
from .base import AsrClient, AsrResult
from .mock import MockAsr


def create_asr(settings: Settings) -> AsrClient:
    provider = settings.asr_provider
    if provider == "mock":
        return MockAsr(settings.mock_asr_text, settings.mock_delay_ms)
    if provider == "bailian":
        from .bailian import BailianAsr

        if not settings.dashscope_api_key:
            raise RuntimeError(
                "ASR_PROVIDER=bailian requires DASHSCOPE_API_KEY "
                "(or set ASR_PROVIDER=mock)"
            )
        return BailianAsr(
            api_key=settings.dashscope_api_key,
            model=settings.asr_model,
            ws_url=settings.dashscope_ws_url or None,
        )
    raise RuntimeError(f"unknown ASR_PROVIDER: {provider}")


__all__ = ["AsrClient", "AsrResult", "MockAsr", "create_asr"]
