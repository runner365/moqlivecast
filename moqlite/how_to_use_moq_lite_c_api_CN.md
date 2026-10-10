# 用 moq-lite C API 写一个推流客户端

[English](how_to_use_moq_lite_c_api.md) | **简体中文**

这份文档面向**要往 `moqlivecast` 推 H.264 + AAC 的 C/C++ 程序**：从建连到逐帧
写出去，把 `moqlite/moq_lite_client.h` 的每个函数讲清楚。

源码：`moqlite/moq_lite_client.{h,c}`（会话状态机）+ `moqlite/moq_lite_wire.{h,c}`
（线编码，可独立编译自测）。
完整可跑的参考实现：`tools/flv2moqlite_client/`（读 FLV 文件 → 按 dts 定时推）。

> **协议版本是 `moq-lite-06`，承载是裸 QUIC**（ALPN `moq-lite-06`）。
> 这条路和浏览器那条 WebTransport（`h3`）是**两套绑定**，服务端在同一个 UDP
> 端口上按 ALPN 分流。本库只做裸 QUIC 那条 —— 它不 offer `h3`，理由是 offer 了
> 却不会说那个协议的话，症状是「连上了但没有任何下文」（见下面「常见问题」）。

---

## 一、整体形状

库是**回调驱动 + 整数句柄**（形态参考 libmoq 的 C API）。调用方拿到的
`moq_lite_session_t` / `moq_lite_broadcast_t` / `moq_lite_track_t` 都是 `uint32_t`
的**句柄**，不是指针 —— 你不可能拿一个已释放的指针去解引用。**0 恒表示无效**。

**事件循环归你**。库不挑一条 loop、也不提供 `uv_run` 的包装：你把
`uv_loop_t *` 传进去，你自己跑它，你自己决定什么时候停（那条 loop 上很可能还有
你的定时器、信号、别的连接）。

推流的调用顺序是固定的四步：

```
① moq_lite_session_new()            建会话（传你自己的 loop）
   moq_lite_session_set_role(PUBLISHER)   ← 必须在 connect 之前
   moq_lite_session_connect()        连接（结果走 on_connected）

② on_connected 里：moq_lite_publish()        声明一条对外广播
                    moq_lite_publish_track()  声明轨（顺带写进 catalog）

③ moq_lite_track_frame()             按时间戳逐帧写（从你的定时器/采集回调里调）

④ moq_lite_track_end() / moq_lite_session_close()   收尾
```

**catalog 与「组」都由库负责，不用你拼。** 你给编解码参数（codec 串 +
avcC/ASC 原字节 + 尺寸/采样率），库自己生成 `catalog.json`、应答对端的订阅、
决定每条轨怎么分组。你只管喂帧。

---

## 二、最小可用骨架

```c
#include "moq_lite_client.h"
#include <uv.h>

static uv_loop_t        *g_loop;
static moq_lite_track_t  g_video, g_audio;

/* 你的 avcC / AudioSpecificConfig 原字节（从 SPS/PPS 与 ASC 拿） */
extern const uint8_t *g_avcc;  extern size_t g_avcc_len;
extern const uint8_t *g_asc;   extern size_t g_asc_len;

static void on_close(moq_lite_session_t s, int err, const char *reason, void *user) {
    (void)s; (void)user;
    fprintf(stderr, "session closed: %d (%s)\n", err, reason ? reason : "");
    uv_stop(g_loop);
}

static void on_connected(moq_lite_session_t s, int status, void *user) {
    (void)user;
    if (status != 0) {
        fprintf(stderr, "connect failed: %d (%s)\n", status,
                moq_lite_strerror(status));
        uv_stop(g_loop);
        return;
    }

    /* ① 声明一条对外广播。path 就是线上广播名。 */
    moq_lite_broadcast_t bc = moq_lite_publish(s, "my_stream.hang");
    if (!bc) { fprintf(stderr, "publish failed\n"); return; }

    /* ② 视频轨。codec 串必须 "avc1." 开头，config 是 avcC 的**原字节**。 */
    moq_lite_rendition_t v = {0};
    v.name = "0.h264";  v.codec = "avc1.42c01e";
    v.config = g_avcc;  v.config_len = g_avcc_len;
    v.width = 640;      v.height = 360;
    g_video = moq_lite_publish_track(bc, /*is_video=*/1, &v);

    /* ③ 音频轨。codec 串必须 "mp4a.40." 开头，config 是 ASC 的原字节。 */
    moq_lite_rendition_t a = {0};
    a.name = "1.aac";   a.codec = "mp4a.40.2";
    a.config = g_asc;   a.config_len = g_asc_len;
    a.sample_rate = 48000;  a.channels = 1;
    g_audio = moq_lite_publish_track(bc, /*is_video=*/0, &a);

    printf("publishing, waiting for subscribers...\n");
    /* 到这里就可以开始喂帧了（见下一节） */
}

int main(void) {
    g_loop = uv_default_loop();

    moq_lite_session_t s = moq_lite_session_new(g_loop);
    if (!s) return 1;

    /* ⚠️ 必须在 connect 之前 —— 它写进 SETUP 的 Role 参数 */
    moq_lite_session_set_role(s, MOQ_LITE_ROLE_PUBLISHER);

    moq_lite_session_cb_t cb = {0};
    cb.on_connected = on_connected;
    cb.on_close     = on_close;
    moq_lite_session_connect(s, "moql://www.webrtcserver.com.cn:4433/anon", &cb);

    uv_run(g_loop, UV_RUN_DEFAULT);   /* 循环归你 */
    return 0;
}
```

---

## 三、逐帧写出去

拿到 `moq_lite_track_t` 之后，**由你**在合适的时间调 `moq_lite_track_frame()`。
库不替你节流 —— 对实时推流来说「什么时候发这一帧」是你的事（`-re` 那种按
dts 定时的做法，参考 `tools/flv2moqlite_client/main.cpp` 里用 `uv_timer` 的
`PumpVideo/PumpAudio`）。

```c
/* 你的编码器每出一帧就调它。**必须在 loop 线程里调**（通常就是你的定时器
 * 或采集回调）。 */
void on_encoded_video(uint64_t dts_us, const uint8_t *avcc, size_t len, int key) {
    moq_lite_track_frame(g_video, dts_us, avcc, len, key);
}
void on_encoded_audio(uint64_t dts_us, const uint8_t *aac, size_t len) {
    moq_lite_track_frame(g_audio, dts_us, aac, len, 0);
}
```

三个参数要点，做错任何一个都是**静默**的后果：

| 参数 | 要求 | 弄错的后果 |
|---|---|---|
| `ts_us` | **绝对微秒**，单调递增。用哪个零点都行（对端只关心差值） | 时间轴乱跳/播放速度离谱 |
| `payload` 视频 | **AVCC**：`[4 字节大端 NALU 长度][NALU]…`。**不是 Annex-B** | 对端 `Invalid NAL unit size` |
| `payload` 音频 | **裸 AAC**，**不带 ADTS 头** | 对端 `Reserved bit set` / 解不出来 |
| `key` | 视频**关键帧必须传 1**，组内其余帧传 0；音频传 0 | **整条视频出不来**（见下） |

**载荷不要带 FLV tag 头，也不要带容器时间戳前缀** —— 那是库的事：库会剥掉你
不该管的东西、加上对端要的东西。你给的就是「这个编码器吐出来的裸帧」。

### `key` 为什么是硬要求

moq-lite 的线上**没有关键帧标志位**。接收端靠一个不变量补：**「组内第一帧即
关键帧」**。库在关键帧上开一个新组、组内其余帧跟着走；**还没开组时到达的非
关键帧会被丢掉**（一组里没有关键帧，整组都解不出来，发出去也是废的）。

所以：

- 关键帧之间如果隔了 10 秒，对端在这 10 秒里新加入就只能等下一个关键帧 ——
  这是 moq-lite 的固有行为，不是 bug；
- **但库替你兜了一层**：它留住「最近一个关键帧起的帧」（音频留最近约 1 秒），
  新订阅方从那里起步，所以后加入的观众不用干等。

---

## 四、API 参考

### 会话

```c
moq_lite_session_t moq_lite_session_new(uv_loop_t *loop);
```
建会话，**不连接**。返回 0 = 失败（`loop` 为空 / OOM）。
`loop` 必须比会话活得久 —— 会话关闭之前不要 `uv_loop_close` 它。

```c
int moq_lite_session_set_role(moq_lite_session_t s, moq_lite_role_t role);
```
设本端角色，**必须在 `connect` 之前**。

| 取值 | 含义 |
|---|---|
| `MOQ_LITE_ROLE_PUBLISHER` (1) | 只推。**推流客户端用这个** |
| `MOQ_LITE_ROLE_SUBSCRIBER` (2) | 只拉（默认）|
| `MOQ_LITE_ROLE_BOTH` (0) | 又推又拉 |

这个值写进 SETUP 的 `Role` 参数，而它的语义是**「对端是谁」**，所以推流端发 1
（对端于是当订阅方，来订阅我们）。**忘了设它 = 服务端只当你是订阅方，一个
ANNOUNCE_REQUEST 都不会发过来，你推的东西没人订阅。**

```c
int moq_lite_session_connect(moq_lite_session_t s, const char *url,
                             const moq_lite_session_cb_t *cb);
```
发起连接（异步），结果走 `cb->on_connected`。返回 0 = 已入队，<0 = 立即失败。

- `url` 形如 `moql://host:port/path`。方案只接受 `moql://` / `moq-lite://`；
  给 `https://` 会返回 `MOQ_LITE_ERR_URL`（那是 WebTransport 那条路，本库不做）。
- **path 决定服务端路由**：`moql://host:4433/anon` 里的 `anon` 就是服务端的入口
  路径（`moqlivecast` 的 `moq_lite_subpath`）。不写 = 服务端默认路径。
- ⚠️ **host 用域名，别用 IP**。`moqlivecast` 的证书没有 IP SAN，换任何做证书
  校验的实现都会以 `NotValidForName` 拒掉（而且线上完全静默）。
- `cb` 会被**深拷贝**，你不必保持它存活。

```c
const char *moq_lite_session_alpn(moq_lite_session_t s);
```
协商到的 ALPN。握手完成后有效。**建议在 `on_connected` 里打出来** —— 它不对
（比如落到 `h3`）说明对端把你当成了 WebTransport。

```c
void moq_lite_session_close(moq_lite_session_t s);
```
关会话（任意线程可调）。之后所有句柄失效；无需再调别的释放函数（幂等）。
`on_close` 是**最后一次**回调。

```c
int moq_lite_session_set_alpn(const char *const *protos, int count);
```
覆盖要 offer 的 ALPN 列表（进程级全局，**须在第一次 connect 之前**调）。
不调就是默认 `{MOQ_LITE_VERSION}` = `{"moq-lite-06"}`。**一般不需要碰它**；
只有在对端认某个你实现了的版本、而默认列表没覆盖时才加。

### 发布

```c
moq_lite_broadcast_t moq_lite_publish(moq_lite_session_t s, const char *path);
```
声明一条对外广播，`path` 就是线上广播名（如 `"my_stream.hang"`）。
**必须在 `connect` 成功之后**调 —— 连接没起来就没有对端可公告。返回 0 = 失败。

```c
moq_lite_track_t moq_lite_publish_track(moq_lite_broadcast_t bc, int is_video,
                                        const moq_lite_rendition_t *r);
```
在广播上声明一条轨，**顺带把它写进 catalog**。`is_video` 决定它进 catalog 的
`video` 还是 `audio` 节。返回 0 = 失败。

**至少要声明一条轨，广播才会被公告出去**：没有 catalog 的广播对端既不会被
通知、也解不开（服务端拿到没有 catalog 的流是直接不服务的）。

`moq_lite_rendition_t` 的字段按媒体类型取：

| 字段 | 视频 | 音频 |
|---|---|---|
| `name` | 轨名，**同时是 catalog `renditions` 的 key** | 同左 |
| `codec` | `"avc1.42c01e"` 等，必须 `avc1.` 开头 | `"mp4a.40.2"`，必须 `mp4a.40.` 开头 |
| `config` / `config_len` | **avcC 的原字节** | **AudioSpecificConfig 的原字节** |
| `width` / `height` | 填 | 忽略 |
| `sample_rate` / `channels` | 忽略 | 填 |
| `bitrate` | 可 0 | 可 0 |

> `config` 是**原字节**，不是十六进制串、也不是 base64。（catalog 的
> `description` 字段用十六进制，那是库自己转的 —— 你不用管。）
> 从 FLV 拿：avcC 就是 AVC sequence header 剥掉 5 字节 tag 头之后的部分。

```c
int moq_lite_track_frame(moq_lite_track_t t, uint64_t ts_us,
                         const uint8_t *payload, size_t len, int key);
```
写一帧。`ts_us` = 绝对微秒。`key` = 视频关键帧传 1。返回 `MOQ_LITE_OK` 或错误码。
**必须在 loop 线程调。**

```c
void moq_lite_track_end(moq_lite_track_t t);
```
收尾：给每条订阅当前开着的组补上结束哨兵 + FIN，并标记该轨结束。推完就调它 ——
少了它，对端那条组流永远不结束。

### 错误码

```c
#define MOQ_LITE_OK              0
#define MOQ_LITE_ERR_ARGS       -1   /* 句柄无效 / 参数为空 */
#define MOQ_LITE_ERR_URL        -2   /* URL 解不了 / 方案不是裸 QUIC */
#define MOQ_LITE_ERR_NOMEM      -3
#define MOQ_LITE_ERR_STATE      -4   /* 会话不在能接受该动作的状态 */
#define MOQ_LITE_ERR_INTERNAL   -5

const char *moq_lite_strerror(int err);
```
`moq_lite_strerror` 是**线程局部**的可读描述，随时可读，不必先有失败。

> ⚠️ **返回句柄的函数失败时返回 `0`，不是负的错误码。** 句柄是 `uint32_t`，
> 把 `-3` 强转过去是个巨大的非零值，`if (!h)` 判不出来。见「常见问题」。

### 线程

| 调用 | 线程 |
|---|---|
| `moq_lite_session_new` / `_set_role` / `_set_alpn` | 任意（connect 之前）|
| `moq_lite_session_connect` / `_close` | 任意线程 |
| `moq_lite_publish` / `_publish_track` / `_track_frame` / `_track_end` | **loop 线程** |
| 所有回调 | loop 线程 |

### 生命周期

回调里**不要**释放会话 —— 返回之后再调 `_close`。库保证「回调期间对象有效、
返回后可能已被下一步动作标记为死」。

---

## 五、编译

库是 CMake 目标 `moqlite_client_core`（静态库，`PUBLIC` 依赖 uv/ssl/crypto）：

```cmake
target_link_libraries(my_pusher PRIVATE moqlite_client_core)
```

`moqlite` 已经在它的 `PUBLIC` include 目录里，直接 `#include "moq_lite_client.h"`
即可。C 和 C++ 都能用（头文件带 `extern "C"`）。

```bash
cmake -B build
cmake --build build --target moqlite_client_core
```

---

## 六、常见问题

都是**静默失败**（不报错、不 reset，只是没下文），按症状查：

| 症状 | 多半是 |
|---|---|
| 连上了，但**对端没有任何反应** | ① 忘了 `set_role(PUBLISHER)` —— 服务端当你是订阅方，不会来订阅你；② ALPN 协商成了 `h3`（打印 `moq_lite_session_alpn()` 确认），服务端把它当 WebTransport，在等一个永远不来的 CONNECT |
| **视频完全出不来，音频正常** | `key` 没传对：关键帧没传 1，库开不了组；或者载荷不是 AVCC（比如误传了 Annex-B）|
| 画面对端报 `Invalid NAL unit size` | 视频载荷里混进了非编解码字节（FLV tag 头 / Annex-B 起始码）|
| 音频报 `Reserved bit set` / `Prediction is not allowed` | 音频载荷带 ADTS 头了（要给裸 AAC），或音视频包串了 |
| **广播没出现在对端**（拉流端看不到） | 一条轨都没声明 —— 没有 catalog 的广播不公告 |
| `moq_lite_publish_track` 报「invalid handle」 | 广播句柄来自**另一个会话**，或 `publish` 其实失败了却当成成功（检查返回值是不是 0）|
| 播到一半卡住 | 收尾没调 `moq_lite_track_end()` |
| 后加入的观众**没有画面** | 正常现象，但只应该等「到下一个关键帧」那么久：库留了最近一个关键帧起的帧；如果等得明显更久，检查 `key` 是否只在关键帧上置 1 |

调试建议：**先把 `moq_lite_session_alpn()` 和每个 `publish*` 的返回值打出来**，
再打一条「已发出 N 帧」。这三行能区分掉上面九成的情况。

---

## 七、反方向（拉流）

同一个库也做订阅方（`on_announce` → `moq_lite_subscribe` → `on_frame`），
只是这份文档聚焦推流。要看那半边：

- `moq_lite_session_announce()` / `moq_lite_subscribe()` / `moq_lite_unsubscribe()`
- `moq_lite_sub_cb_t` 的 `on_frame` / `on_subscribe` 等
- `moq_lite_strip_container_ts()` —— 媒体帧载荷前面垫着一个**绝对微秒**的
  varint 前缀，要用它剥；**catalog 这类数据轨没有这层**，硬剥会静默少 2 字节

头文件里每个函数上面都写了这些坑，注释比这份文档细。

---

## 八、延伸阅读

| 想了解 | 看 |
|---|---|
| 协议细节与实测记录 | `docs/moq-transport-gap.md` §13 / §14 |
| 完整的推流实现（FLV → moq-lite） | `tools/flv2moqlite_client/` |
| 线编码与字节级自测 | `moqlite/moq_lite_wire.h`、`moqlite/moq_lite_wire_test.c` |
