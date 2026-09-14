# WebTransport Chat Meeting 客户端

Vue3 + Vite 实现的 WebTransport 聊天客户端，配合 `wt_chat_server` 服务端使用。

## 功能

- 连接 `https://127.0.0.1:4433/chat?roomid=xxx`，通过 roomid 加入会议（subpath `/chat`）
- 发送消息 → 服务端广播给同 roomid 的所有客户端
- 接收广播消息，显示在消息列表

## 启动

```bash
cd http3/wt_chat_js_client
npm install
npm run dev
```

浏览器打开 `https://127.0.0.1:5173`（Vite dev server 已启用 https）。

## 前置：启动服务端

```bash
./build/wt_chat_server cert/server_cert.pem cert/server_key.pem 127.0.0.1 4433
```

## 浏览器自签证书

WebTransport 需要 HTTPS，服务端用的是自签证书（`cert/server_cert.pem`）。

- 首次访问 `https://127.0.0.1:4433` 会被浏览器拦截，需手动信任证书。
- Chrome：访问后点「高级」→「继续前往」。
- Vite dev server（5173）也用了自签证书，同样需信任一次。

## 消息协议

- 上行（客户端→服务端）：`{"user":"<name>","msg":"<text>"}`
- 下行（服务端→客户端）：`{"from":"<name>","msg":"<text>"}`
  - `from == "system"` 时是系统消息（加入/离开通知）
