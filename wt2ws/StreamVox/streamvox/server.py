from __future__ import annotations

import logging

from fastapi import FastAPI, WebSocket

from .asr import create_asr
from .config import Settings
from .llm.agent import create_agent
from .realtime.session import RealtimeSession
from .tts import create_tts

log = logging.getLogger("streamvox")


def create_app(settings: Settings | None = None) -> FastAPI:
    settings = settings or Settings.load()
    if not logging.getLogger().handlers:
        logging.basicConfig(
            level=logging.INFO,
            format="%(asctime)s %(levelname)-5s %(name)s: %(message)s",
        )

    asr = create_asr(settings)
    tts = create_tts(settings)
    agent = create_agent(settings)

    app = FastAPI(title="StreamVox", version="0.1.0")
    app.state.settings = settings
    app.state.asr = asr
    app.state.tts = tts
    app.state.agent = agent

    log.info("StreamVox config: %s", settings.redacted())

    @app.get("/healthz")
    async def healthz() -> dict:
        return {"ok": True, "service": "streamvox", "config": settings.redacted()}

    @app.websocket("/ws")
    async def websocket_endpoint(ws: WebSocket) -> None:
        await ws.accept()
        session = RealtimeSession(ws, settings, asr, tts, agent)
        log.info("session %s connected", session.session_id)
        try:
            await session.run()
        finally:
            log.info("session %s disconnected", session.session_id)

    return app

