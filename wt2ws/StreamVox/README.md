# StreamVox

**Stream = 流式 WebSocket，Vox = 声音。** 一个用 Python 写的语音 Agent：

```
浏览器麦克风 ──WebSocket(JSON)──> StreamVox ──ASR──> LLM + tools ──TTS──> WebSocket(JSON) ──> 浏览器
```

- WebSocket 事件格式参考 **OpenAI Realtime API**，方便以后直接对接或替换。
- 第一版**不做 VAD**：按住说话，松手提交（push-to-talk）。
- 接入阿里百炼 **ASR**（Paraformer 实时）和 **TTS**（CosyVoice），LLM 用 **LangChain**，支持 tools。
- 默认 `mock` 模式，不需要任何 key 就能把整条链路和浏览器跑通。

## 快速开始（mock，无需 key）

```bash
cd wt2ws/StreamVox
python3.10 -m venv .venv
source .venv/bin/activate
pip install -e ".[dev]"
cp .env.example .env          # 默认 ASR/TTS/LLM 都是 mock
./scripts/run.sh
# 另开一个终端
cd web && npm install && npm run dev
# 浏览器打开 http://127.0.0.1:5173，按住按钮说话
```

mock 模式下：

- ASR 返回 `.env` 里的 `MOCK_ASR_TEXT`；
- LLM 返回一段“收到你说的话”的模拟回复；
- TTS 生成一段 440 Hz 提示音（用来验证浏览器播放链路）。

## Web 客户端：WebSocket / WebTransport

页面顶部可以二选一：

| 方式 | 地址 | 说明 |
|---|---|---|
| WebSocket 直连 | `ws://127.0.0.1:8000/ws` | 默认，浏览器直接连 StreamVox |
| WebTransport（经 proxy） | `https://127.0.0.1:5433/wt2ws` | 浏览器 → wt2ws_proxy → StreamVox |

WT 模式要先启动代理：

```bash
cd /Users/wei.shi/Documents/code/cc_code/moq
./build/wt2ws/wt2ws_proxy \
    --listen 0.0.0.0:5433 \
    --path /wt2ws \
    --backend ws://127.0.0.1:8000/ws \
    --cert cert/server_cert.pem --key cert/server_key.pem \
    --console
```

上面的命令显式用 `cert/server_cert.pem`（mkcert，SAN `localhost` / `127.0.0.1`）。如果
`.zshrc` 里为 moqlivecast 设了 `MOQ_SSL_CERT_FILE`（通常是域名证书），proxy 默认会优先用
它；本地 `https://127.0.0.1:5433` 会证书不匹配，所以这里用 `--cert/--key` 强制 mkcert。
浏览器不信任 mkcert CA 时先执行 `mkcert -install`，或换成已信任的证书。

WT 客户端每 15 秒发 `{"type":"ping"}`，服务端回 `{"type":"pong"}`，用来重置
`wt2ws_proxy` 的应用层 idle（默认 5 分钟）；QUIC 层本身还有约 10 秒的 PING 保活。

注意：

- 5433 是 wt2ws 专用端口，和 moqlivecast 的 4433 分开，可以同时跑；
- 本机浏览器 + 本机 proxy + 本机 StreamVox 都是回环，WT 不会比 WS 快；要测弱网收益，
  需要把 proxy 放到远端，或在浏览器到 proxy 之间加 NLC / `udp_jitter_proxy`；
- 调试时可以直接打开：
  `http://127.0.0.1:5173/?transport=wt&endpoint=https://127.0.0.1:5433/wt2ws`。

## 接入真实服务

```dotenv
ASR_PROVIDER=bailian
TTS_PROVIDER=bailian
LLM_PROVIDER=deepseek

DASHSCOPE_API_KEY=sk-...
ASR_MODEL=paraformer-realtime-v2
TTS_MODEL=cosyvoice-v2
TTS_VOICE=longxiaochun_v2
TTS_SAMPLE_RATE=16000

LLM_BASE_URL=https://api.deepseek.com/v1
LLM_MODEL=deepseek-flash
LLM_API_KEY=sk-...
LLM_THINKING=false
```

重启 `./scripts/run.sh`。`GET /healthz` 会返回脱敏后的当前配置。

### 百炼说明

- ASR 用 `dashscope.audio.asr.Recognition`（`paraformer-realtime-v2`，`format=pcm`，
  `sample_rate=16000`）。**边录边喂**：第一个 `input_audio_buffer.append` 就建立
  识别会话，后续 chunk 直接 `send_audio_frame`；`commit` 时 `stop()` 并只取最终句。
  中间 partial 不展示。`ASR_STREAMING=false` 可退回 commit 后批量喂；流式失败也会
  自动回退到批量。
- TTS 用 `dashscope.audio.tts_v2.SpeechSynthesizer`，按句合成、按 20 ms 分片回传。
  `TTS_SAMPLE_RATE` 支持 8000 / 16000 / 22050 / 24000 / 44100 / 48000，默认
  **16000**（下行 PCM 约 32 KB/s，base64 后约 341 kbps；24k 约 512 kbps）；
- Web 客户端会从 `session.audio.output.format.rate` 读取实际采样率并据此创建
  AudioBuffer，所以把 `TTS_SAMPLE_RATE` 改成 16000 也能正确播放（重启服务并重新连接）。
- 如果控制台给了业务空间专属域名，把它填到 `DASHSCOPE_WS_URL`
  （`wss://<WorkspaceId>.cn-beijing.maas.aliyuncs.com/api-ws/v1/inference`）。
- 模型/音色需要在百炼控制台开通；`cosyvoice-v2` 与 `cosyvoice-v3-plus` 的
  音色名不同，按音色列表填 `TTS_VOICE`。

### LLM / LangChain / tools

DeepSeek V4.1 Flash 的模型名是 **`deepseek-flash`**（旧的 `deepseek-chat` 已不再对应
V4.1）。它默认开启 thinking mode；StreamVox 默认 `LLM_THINKING=false`，原因：
语音对话要低延迟、thinking 模式会忽略 `temperature`，而且带 tools 时要求把
`reasoning_content` 回传（普通 LangChain 循环不会自动做）。需要更强推理时再改成
`true`。


`streamvox/llm/agent.py` 用 `ChatOpenAI(base_url=...)` 接任意 OpenAI 兼容端点：

| Provider | Base URL | 示例模型 |
|---|---|---|
| `deepseek` | `https://api.deepseek.com/v1` | `deepseek-flash`（V4.1 Flash） |
| `qwen` | `https://dashscope.aliyuncs.com/compatible-mode/v1` | `qwen-plus` |
| `openai` | `https://api.openai.com/v1` | `gpt-4o-mini` |

Tools 在 `streamvox/llm/tools.py`，用 LangChain `@tool` 定义，`TOOLS` 列表注册；
`bind_tools()` 后 agent 会流式输出文本，遇到 tool call 就执行、把
`ToolMessage` 追加回上下文，再继续流式生成最终回答。内置示例：

- `get_current_time(timezone)`
- `calculate(expression)`
- `english_word(word)`（演示用词典，方便以后做英语教学 Agent）

## TTS-friendly 输出

系统提示词（`STREAMVOX_INSTRUCTIONS`）已经要求模型输出纯口语文本：不用 Markdown、
列表序号、emoji、链接和装饰符号。TTS 前还会再过一层
[sanitize_for_tts](streamvox/tts/text.py)：

- 去掉 `**`、`#`、`-`/`1.` 列表标记、表格、代码、链接、emoji；
- 换行合并成句号，避免 TTS 念出符号；
- 只清洗送 TTS 的文本，浏览器里显示的仍是大模型原文。

这样即使模型偶尔不听话，TTS 也不会念出“星号星号”或“一、”。

## 协议

`session.update` 采用 GA 的嵌套形状（服务端同时兼容 beta 的扁平字段）：

```json
{
  "type": "session.update",
  "session": {
    "type": "realtime",
    "output_modalities": ["audio"],
    "instructions": "You are StreamVox...",
    "audio": {
      "input":  { "format": { "type": "audio/pcm", "rate": 16000 }, "turn_detection": null },
      "output": { "format": { "type": "audio/pcm", "rate": 16000 }, "voice": "longxiaochun_v2" }
    }
  }
}
```

> OpenAI 官方 Realtime 的 `pcm16` 输入是 **24 kHz**；本项目按需求固定
> **16 kHz**（ASR 也按 16k 识别）。如果以后把同一个客户端直接指向 OpenAI，
> 需要把客户端的 input rate 改成 24000 并做重采样。

### 客户端 → 服务端

| 事件 | 说明 |
|---|---|
| `session.update` | 设置 `instructions` / `voice` / 音频格式；兼容扁平字段和 GA 的 `audio` 嵌套字段 |
| `input_audio_buffer.append` | `{audio: base64(PCM16 16k mono)}`，建议每 100 ms 一包 |
| `input_audio_buffer.commit` | 结束本次录音，触发 ASR |
| `input_audio_buffer.clear` | 丢弃当前缓冲 |
| `conversation.item.create` | 可选：直接发文本 user item |
| `response.create` | 触发 LLM + TTS（会等待 ASR 完成） |
| `response.cancel` | 取消当前回复（实验性） |

### 服务端 → 客户端

| 事件 | 说明 |
|---|---|
| `session.created` / `session.updated` | 会话对象（同时含扁平字段与 `audio` 嵌套块） |
| `input_audio_buffer.committed` | 本次录音的 `item_id` |
| `conversation.item.created` | user / function_call_output item |
| `conversation.item.input_audio_transcription.completed` | 用户语音的 ASR 文本 |
| `response.created` | 开始一次回复 |
| `response.output_text.delta`（beta: `response.text.delta`） | LLM 文本增量 |
| `response.output_audio_transcript.delta`（beta: `response.audio_transcript.delta`） | 语音对应的文本增量 |
| `response.output_audio.delta`（beta: `response.audio.delta`） | base64 PCM16 16k mono（默认，可配 24k） |
| `response.output_audio.done` / `..._transcript.done` / `..._text.done` | 结束事件 |
| `response.done` | `status`: completed / cancelled / failed |
| `error` | `{error: {type, code, message}}` |

事件风格由 `STREAMVOX_EVENT_STYLE=ga|beta` 控制，默认 GA；Web 客户端两种都接受。

**音频格式**：上行 16 kHz 单声道 PCM16（base64），下行 24 kHz 单声道 PCM16
（base64）。与 OpenAI Realtime 的 `pcm16` 语义一致。

## 目录

```
streamvox/
  config.py              # .env 配置
  events.py              # OpenAI Realtime 风格事件构造
  server.py              # FastAPI /ws + /healthz
  realtime/session.py    # 会话状态机：append/commit/response、ASR→LLM→TTS
  asr/{base,mock,bailian}.py
  tts/{base,mock,bailian}.py
  llm/{agent,tools}.py   # LangChain agent + tools
tests/                   # 事件与 mock 端到端测试
web/                     # Vue3 客户端（麦克风、PCM、播放）
scripts/smoke_ws.py      # 不依赖浏览器的 WS 冒烟
```

## 测试

```bash
pytest
python scripts/smoke_ws.py     # 需要先启动 mock 服务
```

`tests/test_session_flow.py` 用 `TestClient` 跑完整的
`append → commit → response.create → response.done`，不需要浏览器或云服务；
覆盖流式 ASR 和 `ASR_STREAMING=false` 两条路径。

`scripts/smoke_ws.py` 模拟按住说话：先发 10 个 100 ms chunk，再 commit，
并打印 `commit → 最终 transcript` 的耗时，可以直接对比
`ASR_STREAMING=true/false`。

## 常见问题

### `CERTIFICATE_VERIFY_FAILED` / `self-signed certificate in certificate chain`

moqlivecast / wt2ws 现在使用 `MOQ_SSL_CERT_FILE` / `MOQ_SSL_KEY_FILE`，不再占用
Python/OpenSSL 的标准 `SSL_CERT_FILE`。如果你之前为 moqlivecast 导出过旧变量，
它会被 Python 当成 CA bundle，导致 DeepSeek/百炼 TLS 失败：

```bash
env | grep SSL_CERT_FILE
# SSL_CERT_FILE=/Users/.../webrtcserver.com.cn_bundle.pem
unset SSL_CERT_FILE SSL_KEY_FILE
```

同时把 shell 配置里的旧变量改成：

```bash
export MOQ_SSL_CERT_FILE=/path/to/fullchain.pem
export MOQ_SSL_KEY_FILE=/path/to/privkey.pem
```

标准 `SSL_CERT_FILE` 应该只用于 CA bundle；StreamVox 不再自动 unset 它。

## 已知限制

- 没有 VAD：交互依赖按键；`turn_detection` 会被忽略。
- 同一时刻只允许一个 response；不支持打断（`response.cancel` 只停新音频，
  浏览器已经排队的声音仍会播完）。
- 不展示 ASR 中间结果，只取最终 text；适合 push-to-talk。
- 默认边录边喂 ASR（`ASR_STREAMING=true`），流式失败自动回退批量。
- TTS 按句调用阻塞式 `call()`；第一句合成完就开始回传，后续句子依次排队。
  若需要更低首包延迟，可换 `streaming_call()`。
- 会话历史只在内存；没有持久化、鉴权、限流。
- 浏览器要求安全上下文（`localhost` 可以）；麦克风会请求
  `echoCancellation / noiseSuppression / autoGainControl`，但最终效果取决于设备。

## 下一步

1. 用 `wt2ws_proxy` 把 `StreamVox` 的最后一公里换成 WebTransport（后端 WS 不变）。
2. 英语教学 Agent：CEFR 分级、发音评测、纠错与进度。
3. VAD / barge-in、会话恢复、鉴权与配额。
