# moqlivecast

[English](README.md) | **简体中文**

一套从零实现 QUIC / HTTP/3 / WebTransport 协议栈的直播流媒体服务。

服务端二进制为 `moqlivecast`。配套的 Vue 前端 Demo（`tools/moq_js_client`）
可以直接在浏览器里推流和拉流。

## 功能

- **MoQ 推流 / 拉流**（基于 WebTransport）
- **WebTransport FLV** 推流 / 拉流
- **HTTP-FLV 拉流**，与 WebTransport 共用同一端口（UDP + TCP）
- **RTMP 推流 / 拉流**（TCP 1935）—— OBS / ffmpeg 推流，任意来源的流也能用 RTMP 拉
- **Android 推流客户端** —— Camera2 + 麦克风 → H.264/AAC → moq-lite（裸 QUIC）
  （见 [Android 使用指南](tools/android/how_to_use_android_moqlit_push.md)）
- **集群（Edge / Center）** —— Edge 把本地流上推到普通 Center 节点，
  本地缺流时按需回源；Edge 上任意播放协议都能播放回源来的流
- 共享 `MediaStreamManager` + 每路流的 **GOP 缓存**：
  新订阅者可以从最近的关键帧开始播放，不用干等下一个关键帧

## 架构

```
   推流端                                    服务端                       拉流端

   浏览器                          ┌──────────────────────┐      浏览器
   WebTransport / HTTPS ──────────>│      moqlivecast     │─────> WebTransport / HTTPS
                                   │          │           │
   ffmpeg ──FLV──> moq ───────────>│          v           │─────> moq ──> ffmpeg
   (libmoq, moq-lite-06)           │ MediaStreamManager   │      (libmoq, moq-lite-06)
                                   │      + GOP 缓存      │
                                   └──────────────────────┘─────> HTTP-FLV 拉流
```

浏览器那条路与 `ffmpeg + moq` 那条路**共用同一个 UDP 端口**：服务端在握手时按
ALPN 分流（`h3` → WebTransport，`moq-lite-06` → 裸 QUIC）。两条路都既能推流也能拉流。

协议栈（QUIC / HTTP/3 / WebTransport）位于 `quic/` 和 `http3/`，
**直接构建在 libuv 与 OpenSSL 之上**——没有依赖任何现成的 QUIC 库。

直播接入、GOP 缓存、各协议的封装与下发位于 `moqlivecast/`。

### 集群拓扑

同一个二进制也能以两级集群运行：**Center** 就是一台普通服务端，任意多个 **Edge** 主动拨它。
Edge 把每路本地流上推到 Center（其他 Edge 才能发现它），只有本地播放器真的请求时才回源拉取：

```text
   Edge 1                                      Center
   ┌──────────────────┐  上推（moq-lite）    ┌──────────────────────┐
   │ 本地推流         │ ───────────────────> │  MediaStreamManager  │
   │ + 本地播放器     │ <─────────────────── │  （普通服务端）      │
   └──────────────────┘  按需回源            └──────────────────────┘
                                                        ^
                                               上推 / 回源（moq-lite）
                                                        │
   Edge 2 ──────────────────────────────────────────────┘
```

Edge↔Center 链路采用 **moq-lite-06（裸 QUIC）**，每路流、每个方向各一条连接。
配置与本机三进程联调见 [集群（Edge / Center）](#集群edge--center)。

## 快速开始

### 编译

依赖 CMake 3.10+ 和 C/C++14 工具链；首次编译需要一些时间
（会一并编译内置的 OpenSSL）。

```bash
cmake -B build
cmake --build build --target moqlivecast
```

产物是 `build/bin/moqlivecast`。

### 配置

`moqlivecast` 启动时接收一个 YAML 配置文件：

```bash
./build/bin/moqlivecast moqlivecast/etc/config.yml
```

如果不方便把证书路径写进 YAML，可以把 `certfile` / `keyfile` 留空，
改用环境变量传入。两个字段都为空时，`moqlivecast` 会读取
`MOQ_SSL_CERT_FILE` 和 `MOQ_SSL_KEY_FILE`；如果仍然为空，会打印原因并退出。这两个变量带 `MOQ_` 前缀，避免和 Python/OpenSSL 客户端使用的标准 `SSL_CERT_FILE` 冲突；旧的 `SSL_CERT_FILE`/`SSL_KEY_FILE` 不再读取。

```bash
export MOQ_SSL_CERT_FILE=/path/to/fullchain.pem
export MOQ_SSL_KEY_FILE=/path/to/privkey.pem
./build/bin/moqlivecast moqlivecast/etc/config.yml
```

> **注意**：WebTransport 要求证书的 hostname 与浏览器里打开的 URL 一致。
> 直接用 `https://127.0.0.1:…` 通常在 Chrome 里会失败——
> 建议配一条 hosts 记录，并确保该域名不走 HTTP 代理。

配置示例：

```yaml
moq_server:
  listenip: 0.0.0.0
  port: 4433
  subpath:
    - desc: media format is flv
      path: flv
    - desc: media format is moq
      path: moq
  certfile: ""
  keyfile: ""

httpflv:
  listenip: 0.0.0.0
  port: 4433

log:
  level: info
  filename: /tmp/moqlivecast.log
  console: false
```

### 浏览器 Demo

```bash
cd tools/moq_js_client
npm install
npm run dev
```

Vite 默认监听 `http://127.0.0.1:5174`。

| 路径 | 用途 |
|---|---|
| `/moq` | MoQ 推流 |
| `/moq-pull` | MoQ 拉流 |
| `/flv` | WebTransport FLV 推流 |
| `/pull` | WebTransport FLV 拉流 |
| `/http-flv` | HTTP-FLV 拉流 |

> 修改 C++ 代码后需要重新编译并重启 `moqlivecast`；
> 浏览器 Demo 不会自动连接新启动的服务端。

### Android 推流 Demo

`tools/android/` 是一个完整的 Android 推流客户端：Camera2 + 麦克风 → MediaCodec
H.264/AAC → `moqlite/` 的 C API → 裸 QUIC（`moq-lite-06`）→ `moqlivecast`。
它同时也是 Android 侧 JNI / 生命周期的参考实现。

```bash
bash tools/android/build_openssl_android.sh        # 每次干净构建前跑一次
cd tools/android && ./gradlew :app:assembleDebug
adb install -r app/build/outputs/apk/debug/app-debug.apk
```

在 App 里填服务端的**局域网 IP**（例如 `moql://192.168.1.143:4433/anon`）和广播名
（默认 `android.hang`），授予摄像头 + 麦克风权限，点 **Start**。然后在浏览器
Demo 的 `/moq-pull` 里拉：

```
https://<server-host>:4433/moq?app=anon&stream=android.hang
```

完整构建选项、源码地图、必须遵守的不变量和排错表：
**[tools/android/how_to_use_android_moqlit_push.md](tools/android/how_to_use_android_moqlit_push.md)**。

### RTMP 推流

RTMP 使用**独立的 TCP 端口**（[`moqlivecast/etc/config.yml`](moqlivecast/etc/config.yml)
里的 `rtmp:` 节，缺省 1935），接入后写入同一份 `MediaStreamManager`，
因此下面所有拉流路径都能直接播放。

```bash
# OBS：服务器 rtmp://<host>:1935/live，串流密钥 abc
ffmpeg -re -i test.flv -c copy -f flv rtmp://<host>:1935/live/abc
```

像普通流一样拉：

```
https://<server-host>:4433/moq?app=live&stream=abc   # 浏览器 /moq-pull
https://<server-host>:4433/live/abc.flv              # HTTP-FLV
```

RTMP server 同时是 egress：`MediaStreamManager` 里的任意流（不论来源）都能用
RTMP 拉，并从缓存的 GOP 起步：

```bash
ffplay rtmp://<host>:1935/live/abc
```

RTMP → **moq-lite**（裸 QUIC，`moq-cli` / Android 客户端）支持 **H.264 + AAC**：
服务端从 FLV 序列头合成 `catalog.json`。但 RTMP 的 **app 必须与 moq-lite
入口路径一致**（缺省 `anon`），因为导出侧按 `<入口>/<broadcast>` 解析：

```bash
# app=anon 推流，然后用 moq-lite 拉
ffmpeg -re -i test.flv -c copy -f flv rtmp://<host>:1935/anon/123456
moq --connect "moql://<host>:4433/anon" --broadcast "123456" export flv | ffplay -
```

其它 codec 仍可走 HTTP-FLV / RTMP，但不会导出到 moq-lite。

### 集群（Edge / Center）

两种角色，同一个二进制：

| 角色 | 配置 | 做什么 |
|---|---|---|
| **Center** | 不用配 —— 保持 `cluster.mode` 为 off | 一台普通服务端：接收 Edge 的上推，并服务 Edge 的回源。 |
| **Edge** | `cluster.mode: edge` + `center_address` | 把本地每路流上推到 Center；本地播放器请求本地没有的流时，从 Center 回源。 |

```yaml
cluster:
  mode: edge                     # off（缺省）= 单机；Center 保持 off
  protocol: moq                  # 第一期只支持 moq-lite-06（裸 QUIC）
  center_address: 10.0.0.1:4433  # Center 的 moq-lite UDP 端口，不是 RTMP/HTTP-FLV
  center_path: anon              # Center 的 moq_lite_subpath 入口
```

Edge 具体做四件事：

- **上推** —— 本地流出现 catalog 时，Edge 向 Center 建一条 `moq-lite-06` 连接，
  发布 catalog 和所有轨。本地 unpublish 后连接会保留 30 秒（避免编码器一重启就断链），
  宽限期结束才关闭。
- **回源** —— 本地播放器请求一条本地没有的流时，Edge 建一条 `moq-lite-06` 连接，
  订阅 catalog 与各轨，把帧写回本机的 `MediaStreamManager`。之后播放走正常本地路径，
  因此 Edge 上**任意**出口都能用：moq-lite、WebTransport、HTTP-FLV、RTMP。
- **发现** —— 一条只读连接订阅 Center 的 announce 流，让本地客户端看得见远端广播。
  没有它，客户端不会发 SUBSCRIBE，也就永远没有回源的触发点。
- **防环** —— 回源来的流不会反向再上推；本地推流优先，一旦本地出现同 key 的
  publisher，回源直接放弃。

只有带 moq-lite catalog 的流才会走这条链路 —— 目前就是 H.264 + AAC，
与 RTMP → moq-lite 导出的规则相同。

用仓库里现成的配置在本机起三个进程
（[`config.yml`](moqlivecast/etc/config.yml)、[`edge1.yml`](moqlivecast/etc/edge1.yml)、
[`edge2.yml`](moqlivecast/etc/edge2.yml)）：

```bash
export MOQ_SSL_CERT_FILE=/path/to/fullchain.pem
export MOQ_SSL_KEY_FILE=/path/to/privkey.pem

./build/bin/moqlivecast moqlivecast/etc/config.yml  # Center：UDP 4433、RTMP 1935、HTTP-FLV 4433
./build/bin/moqlivecast moqlivecast/etc/edge1.yml   # Edge 1：UDP 4434、RTMP 1936、HTTP-FLV 8081
./build/bin/moqlivecast moqlivecast/etc/edge2.yml   # Edge 2：UDP 4435、RTMP 1937、HTTP-FLV 8082
```

往 Edge 1 推流，从 Edge 2 播放（Edge 2 会从 Center 回源）：

```bash
ffmpeg -re -i test.flv -c copy -f flv rtmp://127.0.0.1:1936/anon/123456
ffplay rtmp://127.0.0.1:1937/anon/123456               # Edge 2 按需回源
ffplay https://127.0.0.1:8082/anon/123456.flv          # 同一路流，HTTP-FLV
moq --connect "moql://127.0.0.1:4435/anon" --broadcast 123456 export flv | ffplay -
```

和上面的 RTMP 一节一样，RTMP 的 `app` 必须等于 moq-lite 入口路径（`anon`）：
线上 key 是 `<app>/<stream>`。

所有旋钮都在 [`config.yml`](moqlivecast/etc/config.yml) 的 `cluster:` 节下：

| 配置项 | 缺省 | 含义 |
|---|---|---|
| `reconnect_min_ms` / `reconnect_max_ms` | 500 / 10000 | 出站连接重连退避区间（指数增长，±20% 抖动） |
| `subscribe_timeout_ms` | 3000 | 本地 moq-lite SUBSCRIBE 等待回源出 catalog 的最长时间 |
| `pull_acquire_timeout_ms` | 5000 | 回源拿不到 catalog 的总预算（跨重连不重置） |
| `pull_idle_timeout_ms` | 30000 | 回源多久没有媒体帧就关闭 |
| `max_pull_connections` | 128 | 单 Edge 并发回源上限 |
| `negative_cache_ms` | 5000 | 刚刚回源失败的流，这段时间内不再重试 |

Center 不需要任何集群配置：它只是一台恰好会收到 Edge 连接的普通服务端。

## 线格式：我们的流与规范的对应关系

我们跟随 **[draft-ietf-moq-transport](https://datatracker.ietf.org/doc/draft-ietf-moq-transport/)
§3.3** 的流分类（下面引用的编号来自本仓库当前跟踪的草案版本，控制流的拆分就建立在该版本描述的布局之上）。

规范把消息分成三类，**它们用的流类型并不相同**：

| 类别 | 消息 | 流类型 | 本实现状态 |
|---|---|---|---|
| **控制** | `SETUP` (`0x2f00`)、`GOAWAY` (`0x10`) | **单向** —— 每端各开一条，一对流承载双向语义 | ✅ 已实现 |
| **请求** | `PUBLISH` (`0x1d`)、`SUBSCRIBE` (`0x3`) | **双向** —— 每条请求一条流 | ✅ 已实现 |
| **响应** | `SUBSCRIBE_OK` (`0x4`)、`PUBLISH_DONE` (`0xb`) | **走同一条请求流的反向** —— *不是*新开的流 | ✅ 已实现 |
| **对象** | `SUBGROUP` + `OBJECT` | **单向** | ✅ 已实现 —— 发布方向由 publisher 开流，订阅方向由服务端开流 |

有两点很容易搞错（我们自己一开始就搞错了），值得单独指出：

1. **响应走请求流反向，不走控制流。**
   §3.3.2 原文：*"A request stream is bidirectional and each direction is closed
   independently."* 所以 `SUBSCRIBE_OK` 在 `SUBSCRIBE` 那条请求流的**反方向**上返回。
   草案的消息类型表把它标注为 `Request`（而非 `Control`），这是最权威的判断依据。
   响应**不是**新开一条单向流发出去的。

2. **对象只能走单向流**（*"Objects are sent on unidirectional streams"*），
   这与控制流/请求流的拆分是两个独立的问题。单向流上**没有"回写 alias"这一步**：
   接收端靠 `SUBGROUP` 头里的 Track Alias 判断这批对象属于哪条 track，
   开流的先后顺序不承载任何语义。所以发送方必须主动开流 ——
   发布方向由 publisher 开，订阅方向由服务端开（客户端不再开数据流）。

### 实际表现

`flv2moq_client` 推流时开 **7 条流**：1 条控制（单向，`SETUP`）+ 3 条请求
（双向，catalog/video/audio 各一条 `PUBLISH`）+ 3 条数据流（单向）。
`moq` 拉流时客户端只开 **2 条流**：1 条控制（单向，`SETUP`）+ 1 条请求
（双向，承载两条 `SUBSCRIBE` 并从同一条流读回 `SUBSCRIBE_OK`）；
数据流由**服务端**为每条 track 各开 1 条单向流。

### 与其他实现对照时

如果你在对比不同实现，**第一个该看的是**：对端把 `PUBLISH`/`SUBSCRIBE` 放在
双向流上（正确），还是放在控制单向流上与 `SETUP` 挤在一起（常见的简化做法）。
后者只要两端约定一致也能跑通，但它**不是草案规定的形态，无法互通**。

## 目录结构

| 路径 | 说明 |
|---|---|
| `quic/` `http3/` | C 协议栈（QUIC、HTTP/3、WebTransport），直接构建在 libuv + OpenSSL 之上 |
| `moqlivecast/` | C++ 直播服务端 |
| `moqlivecast/cluster/` | Edge 侧集群：发现、上推、按需回源 |
| `wt2ws/` | WebTransport ↔ WebSocket 中继（最后一公里）—— **[说明](wt2ws/README.md)**（Vue 测试客户端 + Python echo 后端） |
| `wt2ws/StreamVox/` | WebSocket 语音 Agent（兼容 OpenAI Realtime 事件）—— **[说明](wt2ws/StreamVox/README.md)** |
| `moqlite/` | moq-lite-06 客户端 C 库（裸 QUIC）—— **[推流客户端开发指南](moqlite/how_to_use_moq_lite_c_api_CN.md)** · [English](moqlite/how_to_use_moq_lite_c_api.md) |
| `tools/moq_js_client/` | 浏览器推流 / 拉流 Demo |
| `tools/android/` | Android 推流客户端（Camera2 + 麦克风 → moq-lite）—— **[使用指南](tools/android/how_to_use_android_moqlit_push.md)** |
| `tools/flv2moqlite_client/` | FLV 文件 → moq-lite 推流（C++，用的就是 `moqlite/` 的 C API）|
| `http3/wt_chat_js_client/` | WebTransport 信令 Demo（配套 `wt_chat_server`） |
| `udp/` `tls/` | 独立的 UDP / TLS echo 示例（不在服务端主路径上） |
| `3rdparty/` | libuv、OpenSSL、yaml-cpp |

## 当前状态

这是一个**早期公开版本**，不是完整的 IETF MoQ 实现。

**已经可用**：MoQ 推流 / 拉流、WebTransport FLV 推流 / 拉流、HTTP-FLV 拉流、
新订阅者的 GOP 追帧，以及 **RTMP 推流 / 拉流**（OBS / ffmpeg，TCP 1935）。
RTMP 推上来的 H.264 + AAC 流还会通过合成 catalog 导出到 **moq-lite**（裸 QUIC）；
任意来源的流也能用 RTMP 拉。

**集群**（两级 Edge / Center）已可用：Edge 把本地流上推到普通 Center，
把 Center 的广播公告给本地客户端，并在缺流时按需回源 —— Edge 上的 moq-lite、
WebTransport、HTTP-FLV、RTMP 都能播放回源来的流。见
[集群（Edge / Center）](#集群edge--center)。

**尚未完成**：MoQ catalog / 控制平面的完整度（`GOAWAY` 等控制消息目前只解析、不处理）；
catalog 更新对已连接的 moq-lite 订阅方生效；RTMPS 与推流鉴权。

RTMP 的发布端仲裁已与 moq-lite / MOQ 对齐：同一 `app/stream` 同时只有一个
publisher，新连接顶替旧连接（带 10 秒内 3 次的防抖），旧会话停止写帧并被断开；
关闭时只释放自己仍持有的 key。

## 路线图

**集群加固** —— 两级链路已经端到端跑通，但还有几处要补：

- Edge↔Center 链路的 TLS 是 `SSL_VERIFY_NONE`（和仓库里其它客户端一样），
  链路上也没有鉴权；暂时不要把回源端口暴露到不可信网络。
- 每路流、每个方向一条 QUIC 连接。把多路流复用到一个连接上，可以省掉大量
  握手与 NAT 状态开销，适合大规模 Edge。
- 链路协议只实现了 moq-lite（`cluster.protocol: rtmp` 会被拒绝），
  也还没有多级 / mesh 拓扑。

之后是：catalog 与控制平面的完整度、对已连接 moq-lite 订阅方的 catalog 实时更新、
RTMPS 与推流鉴权。

## License

MIT，详见 [LICENSE](LICENSE)。
