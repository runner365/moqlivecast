# HTTP/3 JS Test Tools

两个 JS HTTP/3 测试客户端，**浏览器版本走真正的 QUIC 协议**。

## 前提条件

1. 服务端已启动（端口 4433）：

```bash
cd /Users/wei.shi/Documents/code/cc_code/moq
./build/http3_echo_server cert/server_cert.pem cert/server_key.pem 127.0.0.1 4433
```

2. curl 支持 HTTP/3（Homebrew 安装）：

```bash
/opt/homebrew/opt/curl/bin/curl --version | grep http3
# 预期: ngtcp2/1.24.0 nghttp3/1.18.0
```

## 方式一：浏览器（真正的 H3 客户端）

浏览器原生走 QUIC，是最真实的 HTTP/3 测试。

**启动 Chrome：**

```bash
open -a "Google Chrome" --args \
  --enable-quic \
  --origin-to-force-quic-on=127.0.0.1:4433 \
  http3/http3_js_tools/h3_client.html
```

> `--origin-to-force-quic-on` 强制 Chrome 对该 host 使用 QUIC。
> 如果证书不受信，Chrome 会提示，先手动访问 `https://127.0.0.1:4433` 并点"继续访问"。

**页面自动运行 4 项测试：**

| # | 测试 | 预期结果 |
|---|------|----------|
| 1 | GET `/get/hello?name=alex&age=23` | `{"greeting":"Hello, alex!","age":23}` |
| 2 | POST `/post/hello` (JSON body) | `{"body":"",...}` |
| 3 | POST `/post/hello` (plain text) | `{"body":"",...}` |
| 4 | GET `/nope` → 404 | `404 Not Found` |

也可以单独点每个测试的 `▶ Run` 按钮。

## 方式二：Node.js 脚本

通过 curl 走 HTTP/3，适合命令行和 CI。

```bash
cd http3/http3_js_tools
node h3_client.js https://127.0.0.1:4433
```

> 等效于调用 `h3_client.js` 内嵌的 curl 命令，每项测试打印结果。

## 验证 QUIC 层

在 Chrome 地址栏输入 `chrome://net-internals/#quic` 可以看到活跃的 QUIC session，确认走的是 UDP 4433。

## 其他客户端

```bash
# Go 客户端
cd quic/quicgo_tools && go build -o h3_client h3_client.go
./h3_client https://127.0.0.1:4433

# curl 命令行
/opt/homebrew/opt/curl/bin/curl --http3-only -k -s "https://127.0.0.1:4433/get/hello?name=alex&age=23"

/opt/homebrew/opt/curl/bin/curl --http3-only -k -s -X POST \
  -H 'Content-Type: application/json' \
  -d '{"name":"alex","age":23}' \
  "https://127.0.0.1:4433/post/hello"
```
