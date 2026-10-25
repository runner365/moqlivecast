from __future__ import annotations

import asyncio
import queue
import threading
from typing import Any

from .base import AsrClient, AsrResult, AsrStream


def _sentence_end(RecognitionResult: Any, sentence: dict) -> bool:
    try:
        return bool(RecognitionResult.is_sentence_end(sentence))
    except Exception:
        return True


class BailianAsrStream(AsrStream):
    """One live Paraformer session, fed while the user is speaking.

    All DashScope calls happen on a dedicated thread: `Recognition` owns a
    WebSocket client and is happiest when `start/send/stop` come from one
    thread.  `feed()` only enqueues.
    """

    def __init__(self, api_key: str, model: str, sample_rate: int,
                 ws_url: str | None, loop: asyncio.AbstractEventLoop,
                 timeout_s: float = 30.0):
        self._api_key = api_key
        self._model = model
        self._sample_rate = sample_rate
        self._ws_url = ws_url
        self._loop = loop
        self._timeout = timeout_s

        self._queue: queue.Queue[bytes | None] = queue.Queue()
        self._parts: list[str] = []
        self._errors: list[str] = []
        self._lock = threading.Lock()
        self._done = threading.Event()
        self._closed = False
        self._cancelled = False
        self._request_id = ""
        self._future: asyncio.Future = loop.create_future()
        self._thread = threading.Thread(target=self._run, daemon=True,
                                        name="bailian-asr-stream")
        self._thread.start()

    # ---- thread ----

    def _run(self) -> None:
        recognition = None
        try:
            import dashscope
            from dashscope.audio.asr import (Recognition, RecognitionCallback,
                                             RecognitionResult)

            dashscope.api_key = self._api_key
            if self._ws_url:
                dashscope.base_websocket_api_url = self._ws_url

            parts = self._parts
            errors = self._errors
            lock = self._lock
            done = self._done

            class Callback(RecognitionCallback):
                def on_event(self, result) -> None:
                    try:
                        sentence = result.get_sentence()
                    except Exception:
                        return
                    if not sentence or not sentence.get("text"):
                        return
                    if _sentence_end(RecognitionResult, sentence):
                        with lock:
                            parts.append(sentence["text"])

                def on_complete(self) -> None:
                    done.set()

                def on_error(self, result) -> None:
                    with lock:
                        errors.append(getattr(result, "message", str(result)))
                    done.set()

            recognition = Recognition(
                model=self._model,
                format="pcm",
                sample_rate=self._sample_rate,
                callback=Callback(),
            )
            recognition.start()

            while True:
                chunk = self._queue.get()
                if chunk is None or self._cancelled:
                    break
                recognition.send_audio_frame(chunk)

            recognition.stop()
            if not self._cancelled:
                done.wait(self._timeout)
            try:
                self._request_id = recognition.get_last_request_id()
            except Exception:
                pass
        except Exception as exc:
            with self._lock:
                self._errors.append(f"{type(exc).__name__}: {exc}")
        finally:
            with self._lock:
                if not self._cancelled and not self._errors and not self._done.is_set():
                    self._errors.append("ASR stream finished without on_complete")
            try:
                self._loop.call_soon_threadsafe(self._future.set_result, None)
            except RuntimeError:
                pass

    # ---- API ----

    async def feed(self, pcm16: bytes) -> None:
        if self._closed or not pcm16:
            return
        self._queue.put(pcm16)

    async def finish(self) -> AsrResult:
        if self._closed:
            return AsrResult(text="", raw={"provider": "bailian"})
        self._closed = True
        self._queue.put(None)
        await self._future
        with self._lock:
            errors = list(self._errors)
            text = "".join(self._parts).strip()
        if errors and not text:
            raise RuntimeError("Bailian ASR stream failed: " + "; ".join(errors))
        return AsrResult(
            text=text,
            raw={"provider": "bailian", "streaming": True,
                 "request_id": self._request_id, "errors": errors},
        )

    async def cancel(self) -> None:
        if self._closed:
            return
        self._closed = True
        self._cancelled = True
        self._queue.put(None)
        try:
            await self._future
        except Exception:
            pass


class BailianAsr(AsrClient):
    """Aliyun Bailian (DashScope) real-time ASR.

    The normal path is a live stream (`open_stream`): audio is fed while the
    user speaks, and `finish()` returns the final transcript on commit.  The
    batch `transcribe()` remains as a fallback and for `ASR_STREAMING=false`.
    """

    name = "bailian"
    supports_streaming = True

    def __init__(self, api_key: str, model: str, ws_url: str | None = None,
                 timeout_s: float = 30.0):
        self._api_key = api_key
        self._model = model
        self._ws_url = ws_url
        self._timeout = timeout_s

    async def open_stream(self, sample_rate: int = 16000) -> AsrStream:
        return BailianAsrStream(
            api_key=self._api_key,
            model=self._model,
            sample_rate=sample_rate,
            ws_url=self._ws_url,
            loop=asyncio.get_running_loop(),
            timeout_s=self._timeout,
        )

    async def transcribe(self, pcm16: bytes, sample_rate: int = 16000) -> AsrResult:
        if not pcm16:
            return AsrResult(text="", raw={"provider": "bailian", "empty": True})
        return await asyncio.to_thread(self._transcribe_sync, pcm16, sample_rate)

    def _transcribe_sync(self, pcm16: bytes, sample_rate: int) -> AsrResult:
        import dashscope
        from dashscope.audio.asr import (Recognition, RecognitionCallback,
                                         RecognitionResult)

        dashscope.api_key = self._api_key
        if self._ws_url:
            dashscope.base_websocket_api_url = self._ws_url

        parts: list[str] = []
        errors: list[str] = []
        done = threading.Event()
        lock = threading.Lock()

        class Callback(RecognitionCallback):
            def on_event(self, result) -> None:
                try:
                    sentence = result.get_sentence()
                except Exception:
                    return
                if not sentence or not sentence.get("text"):
                    return
                if _sentence_end(RecognitionResult, sentence):
                    with lock:
                        parts.append(sentence["text"])

            def on_complete(self) -> None:
                done.set()

            def on_error(self, result) -> None:
                with lock:
                    errors.append(getattr(result, "message", str(result)))
                done.set()

        recognition = Recognition(
            model=self._model,
            format="pcm",
            sample_rate=sample_rate,
            callback=Callback(),
        )
        try:
            recognition.start()
            step = 3200  # 100 ms of 16 kHz PCM16
            for i in range(0, len(pcm16), step):
                recognition.send_audio_frame(pcm16[i:i + step])
            recognition.stop()
        except Exception as exc:
            errors.append(f"{type(exc).__name__}: {exc}")
        finally:
            done.wait(self._timeout)

        if errors:
            raise RuntimeError("Bailian ASR failed: " + "; ".join(errors))
        with lock:
            text = "".join(parts).strip()
        request_id = ""
        try:
            request_id = recognition.get_last_request_id()
        except Exception:
            pass
        return AsrResult(text=text, raw={"provider": "bailian", "request_id": request_id})
