# wt2ws_proxy

**WebTransport ↔ WebSocket 中继。** 浏览器/客户端用 WebTransport 接入，服务端继续用普通
WebSocket —— 最后一公里换成 QUIC，后台协议不变。

```
  浏览器 / 客户端                    同一机房（甚至同一台机器）
  ┌──────────────┐  WebTransport   ┌──────────────┐   WebSocket   ┌────────────────┐
  │ StreamVox /  │ ──────────────> │ wt2ws_proxy  │ ────────────> │ py_ws_echo /   │
  │ C 客户端     │ <────────────── │  (C++)       │ <──────────── │ StreamVox      │
  └──────────────┘   h3 / QUIC     └──────────────┘   ws://       └────────────────┘
```

代理**不解析载荷**：它只保留消息边界和 text/binary 类型。JSON、PCM、base64 对它是透明字节。

## 为什么是这层

- 最后一公里是唯一有丢包的一段；代理和后端同机房，第二跳不引入损耗。
- WT 腿走 QUIC：BBR 不因随机丢包砍窗，PTO 比 TCP RTO 快，且没有跨流 HOL。
- 相比 WebRTC，客户端更轻、后端更简单；相比 WS，弱网下上传/下发更稳。

## 目录

```
wt2ws/
  wt2ws_proxy.cpp          # 入口
  wt2ws_config.*           # 命令行配置
  wt2ws_frame.*            # [u32 len][u8 type][payload] 编解码
  wt2ws_backend.*          # 包装仓库里的 WebSocketClient（代理是 WS 客户端）
  wt2ws_session.*          # 一条 WT session <-> 一条 WS 连接
  wt2ws_server.*           # WT 监听、路径路由、session 表、idle 扫描
  tests/wt2ws_smoke.c      # C 客户端端到端冒烟（不依赖浏览器）
  py_ws_echo/              # Python WebSocket echo（测试替身）
  StreamVox/               # Python 语音 Agent + Web 客户端（WS / WT 双模式）
  run_demo.sh              # 起 echo + proxy
```

## 线协议（WT 腿）

WebSocket 是消息语义，WebTransport stream 是字节流，所以代理需要显式分帧：

```
+--------+--------+------------------+
| u32 BE | u8     | payload          |
| length | type   | (length bytes)   |
+--------+--------+------------------+
```

- `length` 只算 payload，不含 5 字节头；上限 `--max-frame`（默认 4 MiB）。
- `type`：1 = text，2 = binary。
- **一条 session 只用一条客户端发起的双向流**，消息天然保序；不要每条消息开一条流。

WS 一侧不需要分帧：一条 WT 消息对应一条 WS message，text/binary 位保持不变。

## 构建

```bash
cmake -B build
cmake --build build --target wt2ws_proxy wt2ws_smoke
```

产物：

```
build/wt2ws/wt2ws_proxy
build/wt2ws/wt2ws_smoke
```

> 注意不在 `build/bin/` 下，`wt2ws/` 是独立子目录。

## 运行

### 1. Python echo

```bash
python3 -m pip install -r wt2ws/py_ws_echo/requirements.txt
python3 wt2ws/py_ws_echo/echo_server.py --port 9001
```

`--delay-ms 200` 可以模拟 ASR/TTS 的处理耗时。

### 2. 代理

```bash
./build/wt2ws/wt2ws_proxy \
    --listen 0.0.0.0:5433 \
    --path /wt2ws \
    --backend ws://127.0.0.1:9001/ \
    --console
```

证书解析顺序与 `moqlivecast` 一致，并多了一层仓库自带证书的兜底：

1. `--cert` / `--key`（必须成对给出）；
2. 环境变量 `MOQ_SSL_CERT_FILE` / `MOQ_SSL_KEY_FILE`（必须成对）；
3. 仓库自带的 `cert/server_cert.pem` / `cert/server_key.pem`（mkcert 生成，
   SAN 为 `localhost` 与 `127.0.0.1`，仅适合本地调试）。

只给一半（例如只设 `MOQ_SSL_CERT_FILE`）时会忽略这两个环境变量，回退到自带证书，
并在日志 source 里标注。这两个变量名带 `MOQ_` 前缀，刻意避开 Python/OpenSSL 标准的
`SSL_CERT_FILE`；旧的 `SSL_CERT_FILE`/`SSL_KEY_FILE` 不再读取。启动日志会打印实际来源：

```
[wt2ws] TLS cert source=env cert=/etc/ssl/example/fullchain.pem
```

本地用 `127.0.0.1` 测试时要注意：如果 `.zshrc` 里为 moqlivecast 设了
`MOQ_SSL_CERT_FILE`（域名证书），proxy 默认会优先用它，和 `127.0.0.1` 不匹配。此时显式
`--cert cert/server_cert.pem --key cert/server_key.pem`，或临时 unset `MOQ_SSL_*`。

生产环境通常这样启动：

```bash
export MOQ_SSL_CERT_FILE=/etc/ssl/example/fullchain.pem
export MOQ_SSL_KEY_FILE=/etc/ssl/example/privkey.pem
./build/wt2ws/wt2ws_proxy --listen 0.0.0.0:5433 --path /wt2ws \
    --backend ws://127.0.0.1:9001/
```

### 3. 冒烟（不需要浏览器）

```bash
./build/wt2ws/wt2ws_smoke 127.0.0.1 5433 /wt2ws
```

覆盖：两条消息合并在一次 WT 写、200 KB 大帧（会被 QUIC 切成很多片）、
1 KB 二进制帧。退出码 0 = 全通过。

### 4. 浏览器客户端

浏览器侧现在由 StreamVox 的 Web 客户端覆盖：

```bash
cd wt2ws/StreamVox/web
npm install
npm run dev
# http://127.0.0.1:5173
```

页面里可以切换：

- **WebTransport → proxy**：默认 `https://127.0.0.1:5433/wt2ws`；
- **WebSocket 直连**：默认 `ws://127.0.0.1:8000/ws`。

纯代理回归（不依赖浏览器/StreamVox）继续用 `wt2ws_smoke` + `py_ws_echo`。

> **证书是 WebTransport 唯一的硬门槛。** 实测 Chrome 的
> `--ignore-certificate-errors` **不会**让 WebTransport 跳过证验证（报
> `certificate unknown`），必须用浏览器信任的证书（`mkcert -install` 或
> 真实域名证书）。页面本身可以是 `http://127.0.0.1:5180`，这是 secure
> context。

## 参数

| 参数 | 默认 | 说明 |
|---|---|---|
| `--listen ip:port` | `0.0.0.0:5433` | WT 监听（IPv4） |
| `--path` | `/wt2ws` | WT CONNECT 的 `:path`，精确匹配 |
| `--cert` / `--key` | `$MOQ_SSL_CERT_FILE` / `$MOQ_SSL_KEY_FILE`，否则 `cert/server_*.pem` | TLS，必须成对 |
| `--backend` | `ws://127.0.0.1:9001/` | 支持 `ws://` / `wss://` |
| `--max-frame` | 4 MiB | 单条消息上限 |
| `--max-queue` | 8 MiB | 每方向待发数据上限，超限关会话 |
| `--idle-ms` | 300000 | 无流量关会话 |
| `--max-sessions` | 1024 | 并发 session 上限 |
| `--connect-timeout-ms` | 5000 | 后端 WS 握手超时 |
| `--log-file` | `/tmp/wt2ws_proxy.log` | 日志 |
| `--console` / `--no-console` | console | 是否同时打 stderr |

## 已验证

- 合帧、200 KB 大帧（分片到达）、二进制帧：**PASS**
- 连续 3 次 + 并发 2 次 session：全部 PASS
- 后端不可达：`--connect-timeout-ms` 后关会话，客户端收到 close
- 客户端直接消失：QUIC idle 20s 后服务端回收 session，后端 socket 释放
- `SIGTERM` 退出干净
- 证书来源：CLI > 环境变量 > 自带证书；只给一半或缺文件都会在启动前报错
- web client：`npm run build` 与 `vue-tsc --noEmit` 均通过（浏览器端 E2E 需真实信任证书）

## 常见问题

- **`Opening handshake failed` / `certificate unknown`**：浏览器不信任证书。
  WebTransport 不走 `--ignore-certificate-errors` 那条路；用
  `mkcert -install` 装上 `cert/` 的 CA，或换成真实域名证书。
- **端口约定**：wt2ws_proxy 默认用 UDP **5433**，刻意与 `moqlivecast` 的 4433
  区分开，两个可以同时跑；如仍冲突，给代理换 `--listen 0.0.0.0:5443`（前端 URL 同步改）。
- **`localhost` 连不上**：代理只监听 IPv4；浏览器可能把 `localhost` 解析到
  `::1`。统一用 `127.0.0.1`（证书 SAN 里有）。
- **echo 起不来**：缺 `websockets`，先
  `python3 -m pip install -r wt2ws/py_ws_echo/requirements.txt`。

## 已知限制

- **没有鉴权**：任何人都能连 WT 并让代理拨后端。公网部署前必须加 token。
- **没有会话恢复**：QUIC 连接迁移未实现；断网后是重建 session，不是续上。
- **一条 session 一条 bidi 流**，不支持多流复用。
- **复用的 WebSocketClient 目前未被仓库其他代码使用**，等于只经过本项目的冒烟验证：
  - `TcpClient` 的连接失败回调被静默吞掉（`OnUVClientConnected` 直接 return），
    所以代理用 `--connect-timeout-ms` 兜底；
  - `WebSocketSessionBase::HandleFrame` 对分片消息逐片回调，不会重组；Python
    `websockets` 不主动分片，所以当前测得通，但换成会分片的后端会破坏消息边界；
  - 101 响应与第一帧同包到达时 `HandleHttpRespone` 会清掉 `data_`，可能丢第一帧。
- **session 关闭后对象保留到进程退出**（避免 libuv/QUIC 关闭回调的 UAF），
  后端 socket 通过延迟 1ms 的 cleanup timer 释放。
- 只监听 IPv4。

## 下一步

1. 鉴权：WT URL token → 后端 `X-Auth`，代理只透传。
2. 会话恢复：`session_id` + 后端保活 10–30s，客户端重连续上。
3. 可选：把 `net/http/websocket` + `utils` 提成顶层共享库，代理拆独立仓库。
4. 在真实浏览器里测 WT vs WS 的上传 RTF / RTT（配合 `udp_jitter_proxy` 或 NLC）。

## 保活

- QUIC 层：HTTP/3 服务端默认开了 keepalive，idle timeout 20s，约每 10s 发 PING；
- 应用层：proxy 的 `--idle-ms` 默认 300000（5 分钟），只看 WT/后端是否有数据；
- StreamVox 的 WT 客户端每 15s 发 `{"type":"ping"}`，服务端回 `pong`，同时重置
  QUIC 和应用层 idle。WS 直连则由 uvicorn 的 WebSocket ping/pong 保活。
