# moqlivecast

[English](README.md) | **简体中文**

一套从零实现 QUIC / HTTP/3 / WebTransport 协议栈的直播流媒体服务。

服务端二进制为 `moqlivecast`。配套的 Vue 前端 Demo（`tools/moq_js_client`）
可以直接在浏览器里推流和拉流。

## 功能

- **MoQ 推流 / 拉流**（基于 WebTransport）
- **WebTransport FLV** 推流 / 拉流
- **HTTP-FLV 拉流**，与 WebTransport 共用同一端口（UDP + TCP）
- 共享 `MediaStreamManager` + 每路流的 **GOP 缓存**：
  新订阅者可以从最近的关键帧开始播放，不用干等下一个关键帧

## 架构

```
浏览器  --WebTransport / HTTPS-->  moqlivecast
                                      |
                                      v
                              MediaStreamManager
                                      |
                         +------------+------------+
                         |            |            |
                      MoQ 推流     WT FLV       HTTP-FLV
                      MoQ 拉流     WT FLV 拉流     拉流
```

协议栈（QUIC / HTTP/3 / WebTransport）位于 `quic/` 和 `http3/`，
**直接构建在 libuv 与 OpenSSL 之上**——没有依赖任何现成的 QUIC 库。

直播接入、GOP 缓存、各协议的封装与下发位于 `moqlivecast/`。

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
`SSL_CERT_FILE` 和 `SSL_KEY_FILE`；如果仍然为空，会打印原因并退出。

```bash
export SSL_CERT_FILE=/path/to/fullchain.pem
export SSL_KEY_FILE=/path/to/privkey.pem
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

## 目录结构

| 路径 | 说明 |
|---|---|
| `quic/` `http3/` | C 协议栈（QUIC、HTTP/3、WebTransport），直接构建在 libuv + OpenSSL 之上 |
| `moqlivecast/` | C++ 直播服务端 |
| `tools/moq_js_client/` | 浏览器推流 / 拉流 Demo |
| `http3/wt_chat_js_client/` | WebTransport 信令 Demo（配套 `wt_chat_server`） |
| `udp/` `tls/` | 独立的 UDP / TLS echo 示例（不在服务端主路径上） |
| `3rdparty/` | libuv、OpenSSL、yaml-cpp |

## 当前状态

这是一个**早期公开版本**，不是完整的 IETF MoQ 实现。

**已经可用**：MoQ 推流 / 拉流、WebTransport FLV 推流 / 拉流、HTTP-FLV 拉流，
以及新订阅者的 GOP 追帧。

**尚未完成**：MoQ catalog / 控制平面的完整度，
以及把仓库里已有的 RTMP 代码接入服务启动流程。

## 路线图

**集群与回源** —— 下一步的重点。

目标是让 `moqlivecast` 能够集群部署：边缘节点按需从源站回源拉流，
一个源站可以支撑多个边缘节点（即标准的直播 CDN 拓扑）。

所需的构件已经在仓库里了：QUIC 客户端 API（`quic/quic_client_api.c`）
和 WebTransport 客户端 API（`http3/webtransport_client_api.c`）。
目前缺的是服务端侧的"回源 + 转推"逻辑。

## License

MIT，详见 [LICENSE](LICENSE)。
