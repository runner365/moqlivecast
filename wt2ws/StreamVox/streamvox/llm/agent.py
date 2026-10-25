from __future__ import annotations

import asyncio
import json
from abc import ABC, abstractmethod
from dataclasses import dataclass
from collections.abc import AsyncIterator
from typing import Any

from langchain_core.messages import AIMessage, HumanMessage, SystemMessage, ToolMessage
from langchain_openai import ChatOpenAI

from ..config import Settings
from .tools import TOOL_MAP, TOOLS


@dataclass
class LlmEvent:
    type: str                 # text | tool_call | tool_result
    text: str = ""
    name: str = ""
    arguments: str = ""
    call_id: str = ""
    result: str = ""


class Agent(ABC):
    @abstractmethod
    async def stream(self, messages: list[dict[str, str]]) -> AsyncIterator[LlmEvent]:
        raise NotImplementedError


def _text_of(content: Any) -> str:
    if isinstance(content, str):
        return content
    if isinstance(content, list):
        parts = []
        for block in content:
            if isinstance(block, str):
                parts.append(block)
            elif isinstance(block, dict) and block.get("type") in ("text", "output_text"):
                parts.append(str(block.get("text", "")))
        return "".join(parts)
    return ""


def _tool_calls_of(message: Any) -> list[dict[str, Any]]:
    calls = getattr(message, "tool_calls", None)
    if calls:
        return list(calls)
    raw = getattr(message, "additional_kwargs", {}) or {}
    raw_calls = raw.get("tool_calls") or []
    out = []
    for call in raw_calls:
        fn = call.get("function") or {}
        out.append({
            "name": fn.get("name", ""),
            "args": json.loads(fn.get("arguments") or "{}"),
            "id": call.get("id", ""),
        })
    return out


class LangChainAgent(Agent):
    """Streaming LangChain agent with tool calls.

    Text deltas are yielded as they arrive.  When the model asks for a tool,
    the tool runs and a ToolMessage is appended before opening the next stream,
    so the final answer is still streamed token by token.
    """

    def __init__(self, settings: Settings):
        self._instructions = settings.instructions
        self._max_rounds = max(1, settings.llm_max_tool_rounds)
        extra = {}
        if settings.llm_provider == "deepseek":
            # DeepSeek V4.1 Flash defaults to thinking mode.  For a voice agent
            # we disable it: lower latency, and temperature actually applies.
            # (Thinking mode also requires passing reasoning_content back with
            # tool calls, which the plain LangChain loop does not do.)
            extra["extra_body"] = {"thinking": {
                "type": "enabled" if settings.llm_thinking else "disabled",
            }}
        base = ChatOpenAI(
            model=settings.llm_model,
            api_key=settings.llm_api_key,
            base_url=settings.llm_base_url,
            temperature=settings.llm_temperature,
            streaming=True,
            **extra,
        )
        self._llm = base.bind_tools(TOOLS) if TOOLS else base

    def _convert(self, messages: list[dict[str, str]]) -> list[Any]:
        out: list[Any] = [SystemMessage(content=self._instructions)]
        for msg in messages:
            role = msg.get("role")
            content = msg.get("content", "")
            if role == "user":
                out.append(HumanMessage(content=content))
            elif role == "assistant":
                out.append(AIMessage(content=content))
        return out

    async def stream(self, messages: list[dict[str, str]]) -> AsyncIterator[LlmEvent]:
        lc_messages = self._convert(messages)
        for _round in range(self._max_rounds):
            acc = None
            async for chunk in self._llm.astream(lc_messages):
                acc = chunk if acc is None else acc + chunk
                text = _text_of(getattr(chunk, "content", ""))
                if text:
                    yield LlmEvent(type="text", text=text)

            if acc is None:
                return
            calls = _tool_calls_of(acc)
            if not calls:
                return

            lc_messages.append(acc)
            for call in calls:
                name = str(call.get("name") or "")
                args = call.get("args") or {}
                call_id = str(call.get("id") or f"call_{_round}_{name}")
                args_json = json.dumps(args, ensure_ascii=False)
                yield LlmEvent(type="tool_call", name=name,
                               arguments=args_json, call_id=call_id)

                tool = TOOL_MAP.get(name)
                if tool is None:
                    result = f"unknown tool: {name}"
                else:
                    try:
                        result = await asyncio.to_thread(tool.invoke, args)
                    except Exception as exc:  # tool errors must not kill the turn
                        result = f"tool error: {type(exc).__name__}: {exc}"
                result = str(result)
                lc_messages.append(ToolMessage(content=result, tool_call_id=call_id))
                yield LlmEvent(type="tool_result", name=name,
                               result=result, call_id=call_id)

        yield LlmEvent(type="text", text="\n（工具调用次数达到上限，先回答到这里。）")


class MockAgent(Agent):
    """No-key mode: echoes the user's sentence with a short canned answer."""

    def __init__(self, settings: Settings):
        self._instructions = settings.instructions

    async def stream(self, messages: list[dict[str, str]]) -> AsyncIterator[LlmEvent]:
        user = ""
        for msg in reversed(messages):
            if msg.get("role") == "user":
                user = msg.get("content", "")
                break
        answer = (
            f"你说的是：“{user}”。"
            "我是 StreamVox 的本地模拟回复。"
            "把 LLM_PROVIDER 改成 deepseek（或 qwen）并填好 key，就会走真实大模型。"
        )
        for i in range(0, len(answer), 4):
            yield LlmEvent(type="text", text=answer[i:i + 4])
            await asyncio.sleep(0.012)


def create_agent(settings: Settings) -> Agent:
    provider = settings.llm_provider
    if provider == "mock":
        return MockAgent(settings)
    if provider not in ("deepseek", "qwen", "openai"):
        raise RuntimeError(f"unknown LLM_PROVIDER: {provider}")
    if not settings.llm_api_key:
        raise RuntimeError(
            f"LLM_PROVIDER={provider} requires LLM_API_KEY (or set LLM_PROVIDER=mock)"
        )
    if provider == "qwen" and "dashscope" not in settings.llm_base_url:
        settings.llm_base_url = "https://dashscope.aliyuncs.com/compatible-mode/v1"
    return LangChainAgent(settings)
