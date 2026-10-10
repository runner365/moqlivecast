# udp_jitter_proxy 使用说明

一个 UDP 弱网模拟器：在浏览器与 `moqlivecast` 之间插入一跳，注入**丢包 / 延时 / 乱序**。

它的目的是让 QUIC 的拥塞控制、重传、Pacing 在受控的劣化环境下跑起来，
方便观察 `quic_cc_bbr.c` 的行为，而不必真的去搭一台弱网机器。

源码：`tools/udp_jitter_proxy.c`（约 350 行，libuv + 裸 UDP，无外部依赖）。

---

## 一、组网图

**它不是透明代理。** 它自己 bind 两个 UDP socket，一进一出，
所以浏览器和服务端**都要知道对应的端口**，不能只改一端。

```
   浏览器                                      udp_jitter_proxy                       moqlivecast
 (web 客户端)                                                                    (服务端, 固定 4433)

       │                                              │                                    │
       │  ① 发往 face-port                            │                                    │
       ├──────────────────────→  :3443 ───────────────┤                                    │
       │                        (socket A)            │                                    │
       │                                              │  ② 从 back-port 转出               │
       │                                              ├──────────────→  :4433  ────────────┤
       │                                              │   (socket B)                       │
       │                                              │                                    │
       │                                              │  ③ 服务端把回包发回 :3444            │
       │                                              │←──────────────  :3444  ←───────────┤
       │                                              │   (socket B)                       │
       │  ④ 从 face-port 回给浏览器                    │                                    │
       │←─────────────────────  :3443  ←──────────────┤                                    │
       │                        (socket A)            │                                    │
       │                                              │                                    │
```

三个要点：

1. **代理只 bind 两个端口，都是它自己的**：`--face-port`（面向浏览器）和
   `--back-port`（面向服务端）。
2. **`--server-port` 不是代理的端口**，是它转发时用的**目的地**（moqlivecast 的端口），
   代理从不 bind 它。
3. **服务端看到的源地址是 `127.0.0.1:3444`**，不是浏览器的地址 —— 所以服务端的回包
   自然发回代理，代理再转给浏览器。这一跳对 QUIC 是可见的（源端口变了），
   但连接本身不受影响。

---

## 二、端口参数速查

| 参数 | 默认值 | 谁 bind | 谁该知道它 |
|---|---|---|---|
| `--face-port` | 3443 | **代理** | **浏览器**（前端 URL 里的端口） |
| `--back-port` | 3444 | **代理** | 只有代理自己（服务端自动回包到这里） |
| `--server-port` | 4433 | moqlivecast | 代理（转发目的地） |
| `--server` | 127.0.0.1 | — | 代理（转发目的地地址） |

> ⚠️ `--server-port` 必须与 `--face-port` / `--back-port` **都不相同**。
> 代码是靠「目标端口 == `--server-port` 就往 back socket 发」来选路的
> （`udp_jitter_proxy.c:173`），撞车会导致回包从错误的 socket 出去。

> ⚠️ `--server` 是**转发目的地址**，不是监听地址。写 `0.0.0.0` 是错的
> （它作为目的地是保留地址，某些系统会侥幸解析成 127.0.0.1，但不会报错，
> 排查时表现为"代理跑了但服务端收不到"）。**这里应当写 `127.0.0.1`。**

---

## 三、编译

```sh
cmake --build build --target udp_jitter_proxy
```

产物在 **`build/udp_jitter_proxy`**（注意不是在 `build/bin/` 下，
那是 `moqlivecast` 的输出目录）。

确认参数可用：

```sh
./build/udp_jitter_proxy --help
```

---

## 四、以 moqlivecast 为例：完整步骤

前提：`moqlivecast` 按 `moqlivecast/etc/config.yml` 监听 **4433**，不需要改动。

### 1. 启动服务端

```sh
./build/bin/moqlivecast moqlivecast/etc/config.yml
```

### 2. 启动代理

用默认端口即可 —— 默认值 `--face-port 3443 --back-port 3444 --server 127.0.0.1
--server-port 4433` 正好就是 moqlivecast 需要的形状，只需补上注入参数：

```sh
./build/udp_jitter_proxy --loss 0.1 --delay-min 0 --delay-max 0 --reorder 0
```

启动后会打印：

```
[proxy] face :3443  <--->  back :3444  →  127.0.0.1:4433
[proxy] loss=10%  delay=0-0ms  reorder=0
```

**用这行输出确认参数真的吃进去了** —— 尤其是 `loss=10%`。

### 3. 改前端的目标端口

`tools/moq_js_client/src/config.ts:12`：

```ts
export const DEFAULT_ORIGIN = 'https://www.webrtcserver.com.cn:3443'
```

从 `:4433` 改成 `:3443`（即 `--face-port` 的值）。重新构建前端后刷新页面。

推流端（`flv2moq_client` 等）同理，URL 里的端口也要指向 `:3443`。

### 4. 自定义端口的情况

如果 `--face-port` 想换别的值，例如 5443：

```sh
./build/udp_jitter_proxy --loss 0.1 --delay-min 0 --delay-max 0 --reorder 0 \
    --face-port 5443
```

前端 `DEFAULT_ORIGIN` 就相应改成 `:5443`。

> 换端口时注意与 `moqlivecast` 的 4433 错开 —— 同一台机器上同一个 UDP 端口
> 只能被一个进程 bind。

---

## 五、只测丢包：必须显式关掉延时和乱序

**默认值会同时注入延时和乱序**，只写 `--loss` 不等于"只丢包"：

| 参数 | 默认 | 只测丢包时 |
|---|---|---|
| `--delay-min` | 5 | **写 0** |
| `--delay-max` | 50 | **写 0** |
| `--reorder` | 3 | **写 0** |

原因见 `udp_jitter_proxy.c:194-219` 的 `maybe_lose_and_delay()`：
丢包判定之后**没有提前返回**，延时是**无条件**加的；`--reorder` 默认 3
还会额外把包扣住直到攒够 3 个。两个源头叠加，纯丢包测试会被污染。

所以「只加 10% 丢包」的完整命令是：

```sh
./build/udp_jitter_proxy --loss 0.1 --delay-min 0 --delay-max 0 --reorder 0
```

残余延时只有 drain 定时器的 1ms 粒度（`:331`），可忽略。

---

## 六、参数参考

| 参数 | 默认 | 说明 |
|---|---|---|
| `--loss X` | `0.10` | 丢包率，**0~1 的小数**。`0.1` = 10%，不是 0.1% |
| `--delay-min N` | `5` | 最小延时 ms |
| `--delay-max N` | `50` | 最大延时 ms，实际延时在闭区间内随机 |
| `--reorder N` | `3` | 乱序缓冲大小，**0 = 关闭** |
| `--face-port N` | `3443` | 面向浏览器的监听端口 |
| `--back-port N` | `3444` | 面向服务端的监听端口 |
| `--server IP` | `127.0.0.1` | 转发目的地址（moqlivecast 所在机器） |
| `--server-port N` | `4433` | 转发目的端口（moqlivecast 的监听端口） |
| `--help` | — | 打印用法 |

---

## 七、注意事项与已知限制

### 1. `--loss` 是「每个方向各丢这么多」

`g_loss_rate` 是单一全局变量，`on_face_recv`（上行）和 `on_back_recv`（下行）
各独立判定一次。所以：

- `--loss 0.1` → 上行 10% + 下行 10%
- 整条链路整体丢包率 ≈ `1 - (1-p)²`，即 **`--loss 0.1` 实际约 19%**

如果你的实验结论要报"链路丢包 10%"，`--loss` 要比 0.1 小
（约 `0.05` 对应整体 10%）。

### 2. 只支持单个客户端

`g_chrome_addr` 是单个全局变量，由**首个收到的包**决定（`:238`）。
推流端和拉流端同时走**同一个**代理实例时，后注册的地址会覆盖前一个，
回包会全部发到最后那一端。

要同时推拉，请跑**两个实例**，端口错开：

```sh
# 实例 1（推流）
./build/udp_jitter_proxy --loss 0.1 --delay-min 0 --delay-max 0 --reorder 0 \
    --face-port 5443 --back-port 5444

# 实例 2（拉流）
./build/udp_jitter_proxy --loss 0.1 --delay-min 0 --delay-max 0 --reorder 0 \
    --face-port 5445 --back-port 5446
```

### 3. bind 失败是静默的

`udp_jitter_proxy.c:312` 与 `:321` 的 `uv_udp_bind()` **没有检查返回值**。
端口被占用时，代理仍会打印启动横幅、进程正常存活，但一个包都收不到，
**日志里没有任何提示**。

现象：代理"看起来跑起来了"，但完全没有流量。
排查：`lsof -iUDP:3443` 看端口到底被谁占着。

### 4. 每次丢包都同步写一行 stderr

```c
fprintf(stderr, "[proxy] DROP %zu bytes → %s:%d\n", ...);
```

stderr 默认无缓冲，每个丢包 = 一次 `write()` 系统调用。包速率高时，
**代理自身的日志开销会混进抖动测量结果**。做精细测量时建议把输出重定向到文件
（如已有的 `tools/run_5pct_test.sh` 那样）并事后统计，或临时改成计数器累加。

### 5. 开启乱序时，缓冲没有超时冲刷

`reorder_flush()` 只在「攒够 N 个」时被调用（`:214`），没有任何超时路径。
流量一旦转稀疏，**最后不足 N 个的残留包会一直卡在缓冲里**，
直到后续包到来才被放出 —— 表现为偶发的、难以解释的延时尖峰。

用 `--reorder 0` 可以完全规避（这也是上面所有示例都带 `--reorder 0` 的原因之一）。
做乱序实验时需要留意这个效应。

---

## 八、参考

- 源码：[`tools/udp_jitter_proxy.c`](udp_jitter_proxy.c)
- 现成的用例脚本：[`tools/run_5pct_test.sh`](run_5pct_test.sh)
  （针对 `wt_echo_server` 的 5% 丢包压测，可作命令行的参考）
- 前端目标地址配置：[`tools/moq_js_client/src/config.ts`](moq_js_client/src/config.ts)
  的 `DEFAULT_ORIGIN`
