import base64

from fastapi.testclient import TestClient

from streamvox.config import Settings
from streamvox.server import create_app


def _settings(style: str = "ga", asr_streaming: bool = True) -> Settings:
    return Settings(
        event_style=style,
        asr_provider="mock",
        asr_streaming=asr_streaming,
        tts_provider="mock",
        llm_provider="mock",
        mock_asr_text="你好，我想练英语。",
        mock_delay_ms=1,
        history_turns=4,
        llm_temperature=0.0,
    )


def _recv_until(ws, wanted: set[str], limit: int = 400) -> list[dict]:
    seen: list[dict] = []
    for _ in range(limit):
        event = ws.receive_json()
        seen.append(event)
        if event.get("type") in wanted:
            break
    return seen


def test_audio_commit_pipeline():
    app = create_app(_settings())
    with TestClient(app) as client:
        with client.websocket_connect("/ws") as ws:
            assert ws.receive_json()["type"] == "session.created"

            ws.send_json({"type": "session.update",
                          "session": {"instructions": "test", "voice": "v"}})
            assert ws.receive_json()["type"] == "session.updated"

            # 0.5 s of silence, 16 kHz mono PCM16
            pcm = b"\x00\x00" * 8000
            ws.send_json({"type": "input_audio_buffer.append",
                          "audio": base64.b64encode(pcm).decode()})
            ws.send_json({"type": "input_audio_buffer.commit"})
            ws.send_json({"type": "response.create"})

            events = _recv_until(ws, {"response.done"})
            types = [e["type"] for e in events]

            assert "input_audio_buffer.committed" in types
            assert "conversation.item.input_audio_transcription.completed" in types
            assert "response.output_audio.delta" in types
            assert "response.output_text.delta" in types
            assert "response.done" in types
            assert events[-1]["response"]["status"] == "completed"

            transcript = next(e["transcript"] for e in events
                              if e["type"].endswith("transcription.completed"))
            assert transcript == "你好，我想练英语。"


def test_text_item_and_beta_event_names():
    app = create_app(_settings("beta"))
    with TestClient(app) as client:
        with client.websocket_connect("/ws") as ws:
            ws.receive_json()  # session.created
            ws.send_json({"type": "conversation.item.create",
                          "item": {"role": "user", "content": [{"type": "input_text",
                                                                "text": "你好"}]}})
            ws.send_json({"type": "response.create"})
            events = _recv_until(ws, {"response.done"})
            types = [e["type"] for e in events]
            assert "response.text.delta" in types
            assert "response.audio.delta" in types
            assert "response.output_text.delta" not in types
            assert events[-1]["response"]["status"] == "completed"


def test_batch_mode_still_works():
    app = create_app(_settings(asr_streaming=False))
    with TestClient(app) as client:
        with client.websocket_connect("/ws") as ws:
            ws.receive_json()  # session.created
            ws.send_json({"type": "input_audio_buffer.append",
                          "audio": base64.b64encode(b"\x00\x00" * 4000).decode()})
            ws.send_json({"type": "input_audio_buffer.commit"})
            ws.send_json({"type": "response.create"})
            events = _recv_until(ws, {"response.done"})
            types = [e["type"] for e in events]
            assert "conversation.item.input_audio_transcription.completed" in types
            assert "response.output_audio.delta" in types
            assert events[-1]["response"]["status"] == "completed"


def test_heartbeat_ping_pong():
    app = create_app(_settings())
    with TestClient(app) as client:
        with client.websocket_connect("/ws") as ws:
            assert ws.receive_json()["type"] == "session.created"
            for kind in ("ping", "heartbeat"):
                ws.send_json({"type": kind})
                pong = ws.receive_json()
                assert pong["type"] == "pong"
                assert pong["event_id"]
