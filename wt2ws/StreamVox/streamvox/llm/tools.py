"""LangChain tools exposed to the voice agent.

Add a function here and put it in TOOLS; `bind_tools()` picks it up and the
agent loop will execute it when the model emits a tool call.
"""

from __future__ import annotations

import ast
import operator
from datetime import datetime
from zoneinfo import ZoneInfo

from langchain_core.tools import tool

_BIN_OPS = {
    ast.Add: operator.add,
    ast.Sub: operator.sub,
    ast.Mult: operator.mul,
    ast.Div: operator.truediv,
    ast.FloorDiv: operator.floordiv,
    ast.Mod: operator.mod,
    ast.Pow: operator.pow,
}


def _safe_eval(node: ast.AST) -> float:
    if isinstance(node, ast.Expression):
        return _safe_eval(node.body)
    if isinstance(node, ast.Constant) and isinstance(node.value, (int, float)):
        return float(node.value)
    if isinstance(node, ast.BinOp) and type(node.op) in _BIN_OPS:
        return _BIN_OPS[type(node.op)](_safe_eval(node.left), _safe_eval(node.right))
    if isinstance(node, ast.UnaryOp):
        if isinstance(node.op, ast.UAdd):
            return +_safe_eval(node.operand)
        if isinstance(node.op, ast.USub):
            return -_safe_eval(node.operand)
    raise ValueError("unsupported expression")


@tool
def get_current_time(timezone: str = "Asia/Shanghai") -> str:
    """Return the current local time. `timezone` is an IANA name like Asia/Shanghai."""
    try:
        now = datetime.now(ZoneInfo(timezone))
    except Exception:
        now = datetime.now(ZoneInfo("UTC"))
        timezone = "UTC"
    return now.strftime(f"%Y-%m-%d %H:%M:%S ({timezone})")


@tool
def calculate(expression: str) -> str:
    """Evaluate a simple arithmetic expression, e.g. '12 * (3 + 4)'."""
    try:
        tree = ast.parse(expression, mode="eval")
        value = _safe_eval(tree)
        if value.is_integer():
            return str(int(value))
        return f"{value:.6g}"
    except Exception as exc:
        return f"calculation error: {exc}"


_ENGLISH_WORDS = {
    "serendipity": "意外发现美好事物的运气；机缘巧合",
    "resilient": "有韧性的；能迅速恢复的",
    "nuance": "细微差别；微妙之处",
    "articulate": "善于表达的；清晰地表达",
    "fluent": "流利的；流畅的",
}


@tool
def english_word(word: str) -> str:
    """Look up a short Chinese gloss for an English word (demo dictionary)."""
    key = word.strip().lower()
    return _ENGLISH_WORDS.get(key, f"no demo entry for '{word}' (connect a real dictionary tool)")


TOOLS = [get_current_time, calculate, english_word]
TOOL_MAP = {t.name: t for t in TOOLS}
