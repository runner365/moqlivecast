"""Environment-backed configuration for StreamVox."""

from __future__ import annotations

import os
from dataclasses import dataclass

from dotenv import load_dotenv

load_dotenv()


def _str(name: str, default: str = "") -> str:
    value = os.getenv(name)
    if value is None:
        return default
    return value.strip()


def _int(name: str, default: int) -> int:
    try:
        return int(_str(name, str(default)))
    except ValueError:
        return default


def _float(name: str, default: float) -> float:
    try:
        return float(_str(name, str(default)))
    except ValueError:
        return default


def _bool(name: str, default: bool) -> bool:
    value = _str(name, "true" if default else "false").lower()
    return value in ("1", "true", "yes", "on")


DEFAULT_INSTRUCTIONS = (
    "You are StreamVox, a friendly voice assistant. Your reply is spoken aloud by TTS, so make it TTS-friendly: use plain conversational text only, with no Markdown (no bold markers, headings, bullet or numbered list markers, tables, code blocks, or links), no emoji, and no decorative symbols. If you enumerate, use natural spoken ordinals (first, second, ... / 第一、第二…) instead of list markers. Use short sentences, write numbers and abbreviations the way they should be read aloud, and keep answers short (2-4 sentences) unless the user asks for more detail."
)


@dataclass
class Settings:
    # server
    host: str = "0.0.0.0"
    port: int = 8000
    event_style: str = "ga"          # ga | beta
    instructions: str = DEFAULT_INSTRUCTIONS
    history_turns: int = 12
    max_audio_bytes: int = 20 * 1024 * 1024

    # ASR
    asr_provider: str = "mock"       # mock | bailian
    asr_model: str = "paraformer-realtime-v2"
    asr_streaming: bool = True       # feed ASR while recording; commit gets the final text
    dashscope_api_key: str = ""
    dashscope_ws_url: str = ""
    mock_asr_text: str = "你好，我想练习一下英语口语。"
    mock_delay_ms: int = 150

    # TTS
    tts_provider: str = "mock"       # mock | bailian
    tts_model: str = "cosyvoice-v2"
    tts_voice: str = "longxiaochun_v2"
    tts_sample_rate: int = 16000
    tts_speech_rate: float = 1.0

    # LLM
    llm_provider: str = "mock"       # mock | deepseek | qwen | openai (all via ChatOpenAI)
    llm_base_url: str = "https://api.deepseek.com/v1"
    llm_model: str = "deepseek-flash"
    llm_api_key: str = ""
    llm_temperature: float = 0.7
    llm_thinking: bool = False   # DeepSeek thinking mode; off by default for voice latency
    llm_max_tool_rounds: int = 4

    @classmethod
    def load(cls) -> "Settings":
        style = _str("STREAMVOX_EVENT_STYLE", "ga").lower()
        if style not in ("ga", "beta"):
            style = "ga"
        return cls(
            host=_str("STREAMVOX_HOST", "0.0.0.0"),
            port=_int("STREAMVOX_PORT", 8000),
            event_style=style,
            instructions=_str("STREAMVOX_INSTRUCTIONS", DEFAULT_INSTRUCTIONS),
            history_turns=_int("STREAMVOX_HISTORY_TURNS", 12),
            max_audio_bytes=_int("STREAMVOX_MAX_AUDIO_BYTES", 20 * 1024 * 1024),
            asr_provider=_str("ASR_PROVIDER", "mock").lower(),
            asr_model=_str("ASR_MODEL", "paraformer-realtime-v2"),
            asr_streaming=_bool("ASR_STREAMING", True),
            dashscope_api_key=_str("DASHSCOPE_API_KEY"),
            dashscope_ws_url=_str("DASHSCOPE_WS_URL"),
            mock_asr_text=_str("MOCK_ASR_TEXT", "你好，我想练习一下英语口语。"),
            mock_delay_ms=_int("ASYNC_MOCK_DELAY_MS", 150),
            tts_provider=_str("TTS_PROVIDER", "mock").lower(),
            tts_model=_str("TTS_MODEL", "cosyvoice-v2"),
            tts_voice=_str("TTS_VOICE", "longxiaochun_v2"),
            tts_sample_rate=_int("TTS_SAMPLE_RATE", 16000),
            tts_speech_rate=_float("TTS_SPEECH_RATE", 1.0),
            llm_provider=_str("LLM_PROVIDER", "mock").lower(),
            llm_base_url=_str("LLM_BASE_URL", "https://api.deepseek.com/v1"),
            llm_model=_str("LLM_MODEL", "deepseek-flash"),
            llm_api_key=_str("LLM_API_KEY"),
            llm_temperature=_float("LLM_TEMPERATURE", 0.7),
            llm_thinking=_bool("LLM_THINKING", False),
            llm_max_tool_rounds=_int("STREAMVOX_LLM_MAX_TOOL_ROUNDS", 4),
        )

    def redacted(self) -> dict:
        return {
            "event_style": self.event_style,
            "asr": self.asr_provider,
            "asr_model": self.asr_model,
            "tts": self.tts_provider,
            "tts_model": self.tts_model,
            "tts_voice": self.tts_voice,
            "llm": self.llm_provider,
            "llm_model": self.llm_model,
            "llm_base_url": self.llm_base_url,
        }
