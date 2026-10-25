from __future__ import annotations

import uvicorn

from .config import Settings
from .server import create_app


def main() -> None:
    settings = Settings.load()
    app = create_app(settings)
    uvicorn.run(app, host=settings.host, port=settings.port, log_level="info")


if __name__ == "__main__":
    main()
