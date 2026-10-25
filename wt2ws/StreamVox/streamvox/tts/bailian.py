from __future__ import annotations

import asyncio
from collections.abc import AsyncIterator

from .base import TtsClient

_PCM_FORMATS = {
    8000: "PCM_8000HZ_MONO_16BIT",
    16000: "PCM_16000HZ_MONO_16BIT",
    22050: "PCM_22050HZ_MONO_16BIT",
    24000: "PCM_24000HZ_MONO_16BIT",
    44100: "PCM_44100HZ_MONO_16BIT",
    48000: "PCM_48000HZ_MONO_16BIT",
}


class BailianTts(TtsClient):
    """Aliyun Bailian CosyVoice TTS via the DashScope Python SDK.

    `call()` is blocking and returns one complete PCM buffer, so it runs in a
    worker thread.  `synthesize()` still yields 20 ms chunks so the WebSocket
    and browser see a streaming response.
    """

    name = "bailian"

    def __init__(self, api_key: str, model: str, voice: str,
                 sample_rate: int = 24000, speech_rate: float = 1.0,
                 ws_url: str | None = None):
        if sample_rate not in _PCM_FORMATS:
            raise RuntimeError(
                f"unsupported TTS_SAMPLE_RATE={sample_rate}; "
                f"choose one of {sorted(_PCM_FORMATS)}"
            )
        self._api_key = api_key
        self._model = model
        self._voice = voice
        self._ws_url = ws_url
        self._speech_rate = speech_rate
        self.sample_rate = sample_rate

    async def synthesize(self, text: str) -> AsyncIterator[bytes]:
        if not text.strip():
            return
        pcm = await asyncio.to_thread(self._call, text)
        for chunk in self.pcm_chunks(pcm, self.sample_rate):
            yield chunk
            await asyncio.sleep(0)

    def _call(self, text: str) -> bytes:
        import dashscope
        from dashscope.audio.tts_v2 import AudioFormat, SpeechSynthesizer

        dashscope.api_key = self._api_key
        if self._ws_url:
            dashscope.base_websocket_api_url = self._ws_url

        fmt = getattr(AudioFormat, _PCM_FORMATS[self.sample_rate])
        synthesizer = SpeechSynthesizer(
            model=self._model,
            voice=self._voice,
            format=fmt,
            speech_rate=self._speech_rate,
        )
        audio = synthesizer.call(text)
        if not audio:
            raise RuntimeError("Bailian TTS returned empty audio")
        return audio
