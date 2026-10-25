"""Make LLM output safe to feed to TTS.

The prompt asks the model for plain spoken text, but models still emit Markdown
lists, bold markers, links and emoji.  Those are read aloud literally by TTS
("star star", "one dot", ...), so strip them before synthesis.  The text shown
in the browser is not changed.
"""

from __future__ import annotations

import re

_EMOJI = re.compile(
    "[\U0001F000-\U0001FAFF\u2600-\u27BF\uFE0F\u200D]"
)


def sanitize_for_tts(text: str) -> str:
    if not text:
        return ""
    s = text.replace("\r\n", "\n").replace("\r", "\n")

    # fenced code / inline code
    s = re.sub(r"```.*?```", " ", s, flags=re.S)
    s = re.sub(r"`([^`]*)`", r"\1", s)

    # images / links / bare URLs
    s = re.sub(r"!\[[^\]]*\]\([^)]*\)", " ", s)
    s = re.sub(r"\[([^\]]+)\]\([^)]*\)", r"\1", s)
    s = re.sub(r"https?://\S+", " ", s)

    # block syntax at line start
    s = re.sub(r"^\s{0,3}#{1,6}\s*", "", s, flags=re.M)
    s = re.sub(r"^\s*>\s?", "", s, flags=re.M)
    s = re.sub(r"^\s*[-*+•·▪◦]\s*", "", s, flags=re.M)
    s = re.sub(r"^\s*\d{1,2}[.)、]\s+", "", s, flags=re.M)
    s = re.sub(r"^\s*[一二三四五六七八九十]+、\s*", "", s, flags=re.M)

    # emphasis / strikethrough / table pipes
    s = re.sub(r"\*\*|__|~~", "", s)
    s = s.replace("*", "").replace("`", "")
    s = re.sub(r"[|^~]", "，", s)

    s = _EMOJI.sub("", s)
    s = re.sub(r"[ \t]+", " ", s)
    s = re.sub(r"\n+", "。", s)
    s = re.sub(r"([：:])([。，])", r"\1", s)
    s = re.sub(r"([。，、；;,])\1+", r"\1", s)
    return s.strip()
