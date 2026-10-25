"""ASGI entry point for `uvicorn streamvox.asgi:app` (optional)."""

from .server import create_app

app = create_app()
