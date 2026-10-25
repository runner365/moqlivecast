"""One WebSocket connection = one voice conversation.

Protocol subset (OpenAI Realtime compatible):

  client -> server
    session.update
    input_audio_buffer.append {audio: base64(pcm16)}
    input_audio_buffer.commit
    input_audio_buffer.clear
    conversation.item.create            (optional text input)
    response.create
    response.cancel

  server -> client
    session.created / session.updated
    input_audio_buffer.committed
    conversation.item.created
    conversation.item.input_audio_transcription.completed
    response.created
    response.output_item.added
    response.content_part.added
    response.output_text.delta / response.text.delta          (GA / beta)
    response.output_audio_transcript.delta / ...transcript... (GA / beta)
    response.output_audio.delta / response.audio.delta        (GA / beta)
    response.output_audio.done / response.audio.done
    response.output_item.done / response.content_part.done
    response.done
    error
"""

from __future__ import annotations

import asyncio
import base64
import binascii
import contextlib
import logging

from fastapi import WebSocket
from starlette.websockets import WebSocketDisconnect

from ..asr.base import AsrClient, AsrStream
from ..config import Settings
from ..llm.agent import Agent
from ..tts.base import TtsClient
from ..tts.text import sanitize_for_tts
from .. import events

log = logging.getLogger("streamvox.session")

_SENTENCE_END = "。！？!?；;\n"


def pop_sentence(buffer: str) -> tuple[str | None, str]:
    """Split the first complete sentence off `buffer`.

    Chinese punctuation ends immediately; an English ". " needs some context
    first, otherwise abbreviations like "Mr. " would cut too early.
    """
    for i, ch in enumerate(buffer):
        if ch in _SENTENCE_END:
            return buffer[:i + 1], buffer[i + 1:]
    idx = buffer.find(". ")
    if idx >= 16:
        return buffer[:idx + 2], buffer[idx + 2:]
    return None, buffer


class RealtimeSession:
    def __init__(self, ws: WebSocket, settings: Settings,
                 asr: AsrClient, tts: TtsClient, agent: Agent):
        self.ws = ws
        self.settings = settings
        self.asr = asr
        self.tts = tts
        self.agent = agent

        self.session_id = events.new_id("sess")
        self.instructions = settings.instructions
        self.voice = settings.tts_voice

        self.audio = bytearray()
        self.history: list[dict[str, str]] = []
        self.last_item_id: str | None = None

        self.pending_item_id: str | None = None
        self.pending_transcript: str | None = None
        self.asr_task: asyncio.Task | None = None
        self.asr_done = asyncio.Event()
        # live ASR for the current (not yet committed) utterance
        self.asr_stream: AsrStream | None = None
        self.active_asr_stream: AsrStream | None = None
        self.asr_stream_checked = False
        self.asr_stream_failed = False

        self.response_task: asyncio.Task | None = None
        self.cancel_event = asyncio.Event()
        self.send_lock = asyncio.Lock()
        self.closed = False

    # ---------- transport ----------

    async def send(self, event: dict) -> None:
        if self.closed:
            return
        async with self.send_lock:
            try:
                await self.ws.send_json(event)
            except Exception:
                self.closed = True

    def _session_object(self) -> dict:
        return events.session_object(
            self.session_id, self.instructions, self.voice,
            self.settings.asr_model, self.tts.sample_rate,
        )

    async def run(self) -> None:
        await self.send(events.session_created(self._session_object()))
        try:
            while True:
                message = await self.ws.receive_json()
                await self.handle(message)
        except WebSocketDisconnect:
            pass
        except Exception as exc:  # malformed JSON, closed socket, ...
            if not isinstance(exc, WebSocketDisconnect):
                log.warning("session %s ended: %s", self.session_id, exc)
        finally:
            await self.shutdown()

    async def shutdown(self) -> None:
        self.closed = True
        self.cancel_event.set()
        for stream in (self.asr_stream, self.active_asr_stream):
            if stream:
                with contextlib.suppress(Exception):
                    await stream.cancel()
        self.asr_stream = None
        self.active_asr_stream = None
        for task in (self.asr_task, self.response_task):
            if task and not task.done():
                task.cancel()
        for task in (self.asr_task, self.response_task):
            if task:
                with contextlib.suppress(asyncio.CancelledError):
                    await task

    # ---------- dispatch ----------

    async def handle(self, message: dict) -> None:
        if not isinstance(message, dict):
            await self.send(events.error("message must be a JSON object"))
            return
        kind = message.get("type")
        if not isinstance(kind, str):
            await self.send(events.error("missing event type"))
            return
        try:
            if kind == "session.update":
                await self._on_session_update(message)
            elif kind == "input_audio_buffer.append":
                await self._on_append(message)
            elif kind == "input_audio_buffer.commit":
                await self._on_commit()
            elif kind == "input_audio_buffer.clear":
                await self._on_clear()
            elif kind == "conversation.item.create":
                await self._on_item_create(message)
            elif kind == "response.create":
                await self._on_response_create()
            elif kind == "response.cancel":
                self.cancel_event.set()
            elif kind in ("ping", "heartbeat"):
                await self.send({"type": "pong", "event_id": events.new_id("evt")})
            else:
                await self.send(events.error(f"unknown event type: {kind}",
                                             code="unknown_event"))
        except Exception as exc:
            log.exception("event %s failed", kind)
            await self.send(events.error(f"{kind} failed: {exc}",
                                         err_type="server_error",
                                         code="internal_error"))

    async def _on_session_update(self, message: dict) -> None:
        session = message.get("session")
        if not isinstance(session, dict):
            await self.send(events.error("session.update requires a session object"))
            return
        if isinstance(session.get("instructions"), str):
            self.instructions = session["instructions"]
        if isinstance(session.get("voice"), str) and session["voice"]:
            self.voice = session["voice"]
        audio = session.get("audio")
        if isinstance(audio, dict):
            output = audio.get("output")
            if isinstance(output, dict) and isinstance(output.get("voice"), str):
                self.voice = output["voice"] or self.voice
            fmt = output.get("format") if isinstance(output, dict) else None
            if isinstance(fmt, dict) and fmt.get("rate"):
                rate = int(fmt["rate"])
                if rate != self.tts.sample_rate:
                    log.info("client requested %d Hz output; TTS runs at %d Hz",
                             rate, self.tts.sample_rate)
        await self.send(events.session_updated(self._session_object()))

    async def _on_append(self, message: dict) -> None:
        payload = message.get("audio")
        if not isinstance(payload, str):
            await self.send(events.error("input_audio_buffer.append requires base64 'audio'"))
            return
        try:
            chunk = base64.b64decode(payload, validate=True)
        except (binascii.Error, ValueError):
            await self.send(events.error("invalid base64 audio"))
            return
        if len(self.audio) + len(chunk) > self.settings.max_audio_bytes:
            await self.send(events.error("audio buffer too large",
                                         code="audio_too_large"))
            return
        self.audio.extend(chunk)
        await self._feed_asr_stream(chunk)

    async def _on_clear(self) -> None:
        if self.asr_stream:
            with contextlib.suppress(Exception):
                await self.asr_stream.cancel()
            self.asr_stream = None
        self.asr_stream_checked = False
        self.asr_stream_failed = False
        self.audio.clear()
        await self.send(events.input_audio_cleared())

    async def _feed_asr_stream(self, chunk: bytes) -> None:
        """Feed the live ASR while the user is still speaking.

        Only the final text is used; partial results are ignored.  Any failure
        here is non-fatal: the buffered PCM is transcribed in batch at commit.
        """
        if not self.settings.asr_streaming or not self.asr.supports_streaming:
            return
        if self.asr_stream_failed:
            return
        if self.asr_stream is None:
            if self.asr_stream_checked:
                return
            self.asr_stream_checked = True
            try:
                self.asr_stream = await self.asr.open_stream(16000)
            except Exception as exc:
                log.warning("cannot open streaming ASR, using batch: %s", exc)
                self.asr_stream_failed = True
                return
            if self.asr_stream is None:
                log.info("ASR: provider %s has no streaming mode; using batch",
                         self.asr.name)
                return
            log.info("ASR: streaming turn opened (provider=%s)", self.asr.name)
        try:
            await self.asr_stream.feed(chunk)
        except Exception as exc:
            log.warning("streaming ASR feed failed, using batch: %s", exc)
            self.asr_stream_failed = True
            with contextlib.suppress(Exception):
                await self.asr_stream.cancel()
            self.asr_stream = None

    async def _on_commit(self) -> None:
        if self.asr_task and not self.asr_task.done():
            await self.send(events.error("previous audio is still being transcribed",
                                         code="asr_in_progress"))
            return
        if not self.audio:
            if self.asr_stream:
                with contextlib.suppress(Exception):
                    await self.asr_stream.cancel()
                self.asr_stream = None
            await self.send(events.error("input audio buffer is empty",
                                         code="input_audio_buffer_commit_empty"))
            return

        item_id = events.new_id("item")
        pcm = bytes(self.audio)
        stream = self.asr_stream
        self.asr_stream = None
        self.asr_stream_checked = False
        self.asr_stream_failed = False
        self.audio.clear()
        self.pending_item_id = item_id
        self.pending_transcript = None
        self.asr_done.clear()
        self.last_item_id = item_id

        await self.send(events.input_audio_committed(item_id))
        await self.send(events.conversation_item_created(events.user_audio_item(item_id)))
        if stream is not None:
            self.active_asr_stream = stream
            self.asr_task = asyncio.create_task(
                self._asr_stream_worker(item_id, stream, pcm))
        else:
            log.info("ASR: batch turn (%d bytes)", len(pcm))
            self.asr_task = asyncio.create_task(self._asr_worker(item_id, pcm))

    async def _deliver_transcript(self, item_id: str, result) -> None:
        text = (result.text or "").strip()
        if not text:
            await self.send(events.error("ASR returned an empty transcript",
                                         err_type="server_error",
                                         code="empty_transcription"))
            return
        self.pending_transcript = text
        self.history.append({"role": "user", "content": text})
        await self.send(events.transcription_completed(item_id, text))

    async def _asr_worker(self, item_id: str, pcm: bytes) -> None:
        try:
            result = await self.asr.transcribe(pcm, 16000)
            await self._deliver_transcript(item_id, result)
        except asyncio.CancelledError:
            raise
        except Exception as exc:
            log.exception("ASR failed")
            await self.send(events.error(f"ASR failed: {exc}",
                                         err_type="server_error",
                                         code="asr_error"))
        finally:
            self.asr_done.set()

    async def _asr_stream_worker(self, item_id: str, stream: AsrStream,
                                 pcm: bytes) -> None:
        try:
            try:
                result = await stream.finish()
            except Exception as exc:
                log.warning("streaming ASR failed, retrying in batch: %s", exc)
                result = await self.asr.transcribe(pcm, 16000)
            await self._deliver_transcript(item_id, result)
        except asyncio.CancelledError:
            with contextlib.suppress(Exception):
                await stream.cancel()
            raise
        except Exception as exc:
            log.exception("ASR failed")
            await self.send(events.error(f"ASR failed: {exc}",
                                         err_type="server_error",
                                         code="asr_error"))
        finally:
            self.active_asr_stream = None
            self.asr_done.set()

    async def _on_item_create(self, message: dict) -> None:
        item = message.get("item")
        if not isinstance(item, dict):
            await self.send(events.error("conversation.item.create requires an item"))
            return
        if item.get("role") != "user":
            # Assistant/tool items from the client are accepted but ignored.
            return
        text = ""
        for part in item.get("content") or []:
            if isinstance(part, dict) and isinstance(part.get("text"), str):
                text += part["text"]
        text = text.strip()
        if not text:
            await self.send(events.error("user item has no text"))
            return
        item_id = item.get("id") or events.new_id("item")
        self.pending_item_id = item_id
        self.pending_transcript = text
        self.last_item_id = item_id
        self.history.append({"role": "user", "content": text})
        await self.send(events.conversation_item_created({
            "id": item_id,
            "object": "realtime.item",
            "type": "message",
            "role": "user",
            "status": "completed",
            "content": [{"type": "input_text", "text": text}],
        }))

    async def _on_response_create(self) -> None:
        if self.response_task and not self.response_task.done():
            await self.send(events.error("a response is already in progress",
                                         code="response_in_progress"))
            return
        if self.asr_task and not self.asr_done.is_set():
            try:
                await asyncio.wait_for(self.asr_done.wait(), timeout=60)
            except asyncio.TimeoutError:
                await self.send(events.error("timed out waiting for transcription",
                                             err_type="server_error",
                                             code="asr_timeout"))
                return
        if not self.pending_item_id:
            await self.send(events.error("no user item; call commit first",
                                         code="no_user_item"))
            return
        if not self.pending_transcript:
            await self.send(events.error("transcript is not ready",
                                         code="transcription_not_ready"))
            return
        # A response consumes the pending user item; a second response.create
        # without a new commit must not silently answer the same turn again.
        self.pending_item_id = None
        self.pending_transcript = None
        self.response_task = asyncio.create_task(self._run_response())

    # ---------- response pipeline ----------

    async def _run_response(self) -> None:
        response_id = events.new_id("resp")
        item_id = events.new_id("item")
        self.cancel_event.clear()

        await self.send(events.response_created(response_id))
        await self.send(events.output_item_added(
            response_id, events.assistant_message_item(item_id)))
        await self.send(events.content_part_added(response_id, item_id))

        sentences: asyncio.Queue[str | None] = asyncio.Queue()
        tts_task = asyncio.create_task(self._tts_worker(sentences, response_id, item_id))
        full_text = ""
        buffer = ""
        cancelled = False
        output_index = 1

        try:
            async for item in self.agent.stream(self.history):
                if self.cancel_event.is_set():
                    cancelled = True
                    break
                if item.type == "text" and item.text:
                    full_text += item.text
                    buffer += item.text
                    await self.send(events.text_delta(
                        self.settings.event_style, response_id, item_id, item.text))
                    await self.send(events.audio_transcript_delta(
                        self.settings.event_style, response_id, item_id, item.text))
                    while True:
                        sentence, buffer = pop_sentence(buffer)
                        if not sentence:
                            break
                        await sentences.put(sentence)
                elif item.type == "tool_call":
                    await self.send(events.function_call_item_added(
                        response_id, item.call_id, item.name, output_index))
                    await self.send(events.function_call_arguments_done(
                        response_id, item.call_id, item.arguments, output_index))
                    output_index += 1
                elif item.type == "tool_result":
                    await self.send(events.conversation_item_created(
                        events.function_call_output_item(
                            item.call_id, item.name, item.result)))

            if not cancelled and buffer.strip():
                await sentences.put(buffer)
            await sentences.put(None)
            await tts_task
        except asyncio.CancelledError:
            cancelled = True
            raise
        except Exception as exc:
            log.exception("response failed")
            await sentences.put(None)
            with contextlib.suppress(Exception):
                await tts_task
            await self.send(events.error(f"response failed: {exc}",
                                         err_type="server_error",
                                         code="response_error"))
            await self.send(events.response_done(
                response_id, "failed", {"type": "error", "error": {"message": str(exc)}}))
            self.response_task = None
            return

        if full_text.strip() and not cancelled:
            self.history.append({"role": "assistant", "content": full_text.strip()})
            self._trim_history()

        await self.send(events.text_done(
            self.settings.event_style, response_id, item_id, full_text))
        await self.send(events.audio_transcript_done(
            self.settings.event_style, response_id, item_id, full_text))
        await self.send(events.audio_done(self.settings.event_style, response_id, item_id))
        part = {"type": "audio", "transcript": full_text}
        await self.send(events.content_part_done(response_id, item_id, part))
        done_item = events.assistant_message_item(item_id)
        done_item["status"] = "completed"
        done_item["content"] = [part]
        await self.send(events.output_item_done(response_id, done_item))
        await self.send(events.response_done(
            response_id, "cancelled" if cancelled else "completed"))
        self.response_task = None

    async def _tts_worker(self, queue: asyncio.Queue, response_id: str,
                          item_id: str) -> None:
        try:
            while True:
                sentence = await queue.get()
                if sentence is None:
                    return
                if self.cancel_event.is_set():
                    continue
                spoken = sanitize_for_tts(sentence)
                if not spoken:
                    continue
                async for pcm in self.tts.synthesize(spoken):
                    if self.cancel_event.is_set():
                        break
                    if pcm:
                        await self.send(events.audio_delta(
                            self.settings.event_style, response_id, item_id,
                            base64.b64encode(pcm).decode("ascii")))
        except asyncio.CancelledError:
            raise
        except Exception as exc:
            log.exception("TTS failed")
            await self.send(events.error(f"TTS failed: {exc}",
                                         err_type="server_error",
                                         code="tts_error"))

    def _trim_history(self) -> None:
        limit = max(1, self.settings.history_turns) * 2
        if len(self.history) > limit:
            self.history = self.history[-limit:]
