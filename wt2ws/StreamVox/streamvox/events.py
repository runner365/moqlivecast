"""OpenAI Realtime compatible event builders.

The client/server event names changed between the beta and GA Realtime API:

    beta                          GA
    response.audio.delta          response.output_audio.delta
    response.audio.done           response.output_audio.done
    response.audio_transcript.*   response.output_audio_transcript.*
    response.text.*               response.output_text.*

StreamVox defaults to the GA names (STREAMVOX_EVENT_STYLE=ga) and can emit the
beta names with STREAMVOX_EVENT_STYLE=beta.  The web client accepts both.
"""

from __future__ import annotations

import uuid
from typing import Any

GA = "ga"
BETA = "beta"


def new_id(prefix: str) -> str:
    return f"{prefix}_{uuid.uuid4().hex[:24]}"


def _name(style: str, ga: str, beta: str) -> str:
    return beta if style == BETA else ga


def session_object(session_id: str, instructions: str, voice: str,
                   asr_model: str, output_sample_rate: int = 16000) -> dict[str, Any]:
    """The subset of the session object StreamVox understands.

    Both the flat (beta-era) fields and the nested `audio` block (GA-era) are
    included, so a client built against either shape can read what it needs.
    """
    return {
        "id": session_id,
        "object": "realtime.session",
        "type": "realtime",
        "model": "streamvox",
        # beta-era field kept for older clients; GA uses output_modalities
        "modalities": ["text", "audio"],
        "output_modalities": ["audio"],
        "instructions": instructions,
        "voice": voice,
        "input_audio_format": "pcm16",
        "output_audio_format": "pcm16",
        "input_audio_transcription": {"model": asr_model},
        "turn_detection": None,
        "audio": {
            "input": {
                "format": {"type": "audio/pcm", "rate": 16000},
                "turn_detection": None,
                "transcription": {"model": asr_model},
            },
            "output": {
                "format": {"type": "audio/pcm", "rate": output_sample_rate},
                "voice": voice,
            },
        },
    }


def session_created(session: dict[str, Any]) -> dict[str, Any]:
    return {"type": "session.created", "event_id": new_id("evt"), "session": session}


def session_updated(session: dict[str, Any]) -> dict[str, Any]:
    return {"type": "session.updated", "event_id": new_id("evt"), "session": session}


def input_audio_committed(item_id: str, previous_item_id: str | None = None) -> dict[str, Any]:
    return {
        "type": "input_audio_buffer.committed",
        "event_id": new_id("evt"),
        "previous_item_id": previous_item_id,
        "item_id": item_id,
    }


def input_audio_cleared() -> dict[str, Any]:
    return {"type": "input_audio_buffer.cleared", "event_id": new_id("evt")}


def user_audio_item(item_id: str) -> dict[str, Any]:
    return {
        "id": item_id,
        "object": "realtime.item",
        "type": "message",
        "role": "user",
        "status": "in_progress",
        "content": [{"type": "input_audio", "transcript": None}],
    }


def assistant_message_item(item_id: str) -> dict[str, Any]:
    return {
        "id": item_id,
        "object": "realtime.item",
        "type": "message",
        "role": "assistant",
        "status": "in_progress",
        "content": [],
    }


def conversation_item_created(item: dict[str, Any]) -> dict[str, Any]:
    return {"type": "conversation.item.created", "event_id": new_id("evt"), "item": item}


def transcription_completed(item_id: str, transcript: str,
                            usage: dict[str, Any] | None = None) -> dict[str, Any]:
    return {
        "type": "conversation.item.input_audio_transcription.completed",
        "event_id": new_id("evt"),
        "item_id": item_id,
        "content_index": 0,
        "transcript": transcript,
        "usage": usage or {},
    }


def response_object(response_id: str, status: str = "in_progress",
                    status_details: dict[str, Any] | None = None) -> dict[str, Any]:
    return {
        "id": response_id,
        "object": "realtime.response",
        "status": status,
        "status_details": status_details,
        "output": [],
        "usage": None,
    }


def response_created(response_id: str) -> dict[str, Any]:
    return {
        "type": "response.created",
        "event_id": new_id("evt"),
        "response": response_object(response_id),
    }


def output_item_added(response_id: str, item: dict[str, Any],
                      output_index: int = 0) -> dict[str, Any]:
    return {
        "type": "response.output_item.added",
        "event_id": new_id("evt"),
        "response_id": response_id,
        "output_index": output_index,
        "item": item,
    }


def content_part_added(response_id: str, item_id: str,
                       part: dict[str, Any] | None = None,
                       output_index: int = 0, content_index: int = 0) -> dict[str, Any]:
    return {
        "type": "response.content_part.added",
        "event_id": new_id("evt"),
        "response_id": response_id,
        "item_id": item_id,
        "output_index": output_index,
        "content_index": content_index,
        "part": part or {"type": "audio", "transcript": ""},
    }


def text_delta(style: str, response_id: str, item_id: str, delta: str,
               output_index: int = 0, content_index: int = 0) -> dict[str, Any]:
    return {
        "type": _name(style, "response.output_text.delta", "response.text.delta"),
        "event_id": new_id("evt"),
        "response_id": response_id,
        "item_id": item_id,
        "output_index": output_index,
        "content_index": content_index,
        "delta": delta,
    }


def text_done(style: str, response_id: str, item_id: str, text: str,
              output_index: int = 0, content_index: int = 0) -> dict[str, Any]:
    return {
        "type": _name(style, "response.output_text.done", "response.text.done"),
        "event_id": new_id("evt"),
        "response_id": response_id,
        "item_id": item_id,
        "output_index": output_index,
        "content_index": content_index,
        "text": text,
    }


def audio_transcript_delta(style: str, response_id: str, item_id: str, delta: str,
                           output_index: int = 0, content_index: int = 0) -> dict[str, Any]:
    return {
        "type": _name(style, "response.output_audio_transcript.delta",
                      "response.audio_transcript.delta"),
        "event_id": new_id("evt"),
        "response_id": response_id,
        "item_id": item_id,
        "output_index": output_index,
        "content_index": content_index,
        "delta": delta,
    }


def audio_transcript_done(style: str, response_id: str, item_id: str, transcript: str,
                          output_index: int = 0, content_index: int = 0) -> dict[str, Any]:
    return {
        "type": _name(style, "response.output_audio_transcript.done",
                      "response.audio_transcript.done"),
        "event_id": new_id("evt"),
        "response_id": response_id,
        "item_id": item_id,
        "output_index": output_index,
        "content_index": content_index,
        "transcript": transcript,
    }


def audio_delta(style: str, response_id: str, item_id: str, delta_b64: str,
                output_index: int = 0, content_index: int = 0) -> dict[str, Any]:
    return {
        "type": _name(style, "response.output_audio.delta", "response.audio.delta"),
        "event_id": new_id("evt"),
        "response_id": response_id,
        "item_id": item_id,
        "output_index": output_index,
        "content_index": content_index,
        "delta": delta_b64,
    }


def audio_done(style: str, response_id: str, item_id: str,
               output_index: int = 0, content_index: int = 0) -> dict[str, Any]:
    return {
        "type": _name(style, "response.output_audio.done", "response.audio.done"),
        "event_id": new_id("evt"),
        "response_id": response_id,
        "item_id": item_id,
        "output_index": output_index,
        "content_index": content_index,
    }


def content_part_done(response_id: str, item_id: str, part: dict[str, Any] | None = None,
                      output_index: int = 0, content_index: int = 0) -> dict[str, Any]:
    return {
        "type": "response.content_part.done",
        "event_id": new_id("evt"),
        "response_id": response_id,
        "item_id": item_id,
        "output_index": output_index,
        "content_index": content_index,
        "part": part or {"type": "audio", "transcript": ""},
    }


def output_item_done(response_id: str, item: dict[str, Any],
                     output_index: int = 0) -> dict[str, Any]:
    return {
        "type": "response.output_item.done",
        "event_id": new_id("evt"),
        "response_id": response_id,
        "output_index": output_index,
        "item": item,
    }


def response_done(response_id: str, status: str = "completed",
                  status_details: dict[str, Any] | None = None) -> dict[str, Any]:
    resp = response_object(response_id, status=status, status_details=status_details)
    return {"type": "response.done", "event_id": new_id("evt"), "response": resp}


def function_call_item_added(response_id: str, call_id: str, name: str,
                             output_index: int) -> dict[str, Any]:
    item = {
        "id": call_id,
        "object": "realtime.item",
        "type": "function_call",
        "status": "in_progress",
        "name": name,
        "call_id": call_id,
        "arguments": "",
    }
    return output_item_added(response_id, item, output_index=output_index)


def function_call_arguments_done(response_id: str, call_id: str, arguments: str,
                                 output_index: int = 0) -> dict[str, Any]:
    return {
        "type": "response.function_call_arguments.done",
        "event_id": new_id("evt"),
        "response_id": response_id,
        "item_id": call_id,
        "output_index": output_index,
        "arguments": arguments,
    }


def function_call_output_item(call_id: str, name: str, output: str) -> dict[str, Any]:
    return {
        "id": new_id("item"),
        "object": "realtime.item",
        "type": "function_call_output",
        "status": "completed",
        "name": name,
        "call_id": call_id,
        "output": output,
    }


def error(message: str, err_type: str = "invalid_request_error",
          code: str | None = None) -> dict[str, Any]:
    event_id = new_id("evt")
    return {
        "type": "error",
        "event_id": event_id,
        "error": {
            "type": err_type,
            "code": code,
            "message": message,
            "event_id": event_id,
        },
    }
