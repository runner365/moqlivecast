# py_ws_echo

最小 WebSocket echo 服务，作为 `wt2ws_proxy` 的后端测试替身。

**它故意不解析载荷**：收到什么就原样发回（可加固定延迟）。这样代理保持通用，
客户端也可以直接用它做 WS 直连 baseline。

```bash
python3 -m pip install -r requirements.txt
python3 echo_server.py --host 127.0.0.1 --port 9001
# 模拟 ASR/TTS 处理耗时：
python3 echo_server.py --port 9001 --delay-ms 200
```

| 参数 | 默认 | 说明 |
|---|---|---|
| `--host` | `127.0.0.1` | 监听地址 |
| `--port` | `9001` | 监听端口 |
| `--delay-ms` | `0` | 回显前的固定延迟 |
| `--max-size` | 8 MiB | 单条消息上限 |

日志会打印每条消息的序号、text/binary、字节数；text 如果是 JSON 还会带
`type`/`seq`，方便和浏览器日志对照。
