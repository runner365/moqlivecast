/* ============================================================
 * flv2moqlite_client —— FLV 文件 → moq-lite-06 推流（C++）
 *
 * 这个工具的目的是**验证 moqlite 的 C API**（moqlite/moq_lite_client.{h,c}）。
 * 它是订阅侧 demo（已删的 tools/moqlite_client）的反方向：那个从服务端拉，
 * 这个往服务端推。
 *
 * 三件事：
 *   ① 解 FLV —— 直接复用 moqlivecast/format/flv/flv_demux.cpp + 本仓既有的
 *      tools/flv2moq_client/flv_reader.{hpp,cpp}（它已经封好：剥 5B/2B FLV tag
 *      头、过滤 H.264/AAC、给出 is_seq_hdr/key/dts/cts/payload）。
 *      **不重写解封装** —— 服务端拼回 FLV 的偏移就是按它写的。
 *   ② 按 dts 准时发 —— 用 uv_timer 挂在**调用方自己的 loop** 上（库不拥有
 *      循环）。锚点 = 首帧 dts ↔ 首帧墙钟，之后每帧的到期时刻 =
 *      锚点墙钟 + (dts - 锚点 dts) / speed。滞后超过阈值的非关键帧直接丢。
 *   ③ 调 moqlite 的 C API —— set_role(PUBLISHER) → publish → publish_track →
 *      逐帧 track_frame → track_end。
 *
 * ⚠️ 用法里的 host 用**域名**，别用 127.0.0.1：服务端证书没有 IP SAN，做验证
 * 的实现（rustls/libmoq）会以 NotValidForName 拒掉且线上静默。（本 API 的 TLS
 * 是 SSL_VERIFY_NONE，IP 也能连 —— 但别养成习惯。）
 * ============================================================ */

#include "flv_reader.hpp"          /* tools/flv2moq_client 的那份，逐字复用 */
#include "moq_lite_client.h"       /* C API（extern "C" 在头里） */
#include "logger.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <string>

#include <uv.h>

namespace {

/* ── 配置 ───────────────────────────────────── */

struct Config {
    std::string url;          /* moql://host:port/path */
    std::string flv_path;
    std::string broadcast;    /* 线上广播名 */
    std::string vtrack = "0.flv-v";
    std::string atrack = "1.flv-a";
    double      speed = 1.0;  /* 播放倍速（1.0 = 实时）*/
    int         seconds = 0;  /* >0 = 只推这么多秒（0 = 整文件）*/
};

/* 按 dts 节流。**不是** sleep 循环 —— 那是阻塞 uv loop，QUIC 的发包/重传
 * 全都会被卡住。这里用 1ms 的 uv_timer 驱动。 */
constexpr uint64_t kTickMs      = 1;
constexpr int64_t  kMaxLateMs   = 500;   /* 落后这么多就丢非关键帧 */
constexpr int64_t  kResyncLateMs = 3000; /* 关键帧落后这么多 → 重锚，别丢 */
constexpr size_t   kMaxPending  = 512;   /* 两条队列合计的帧数上限（内存闸）*/

struct App {
    Config cfg;

    uv_loop_t   *loop = nullptr;
    uv_timer_t   tick{};

    moq_lite_session_t sess = 0;
    moq_lite_track_t   vt = 0;
    moq_lite_track_t   at = 0;
    int                announced = 0;

    /* 解出的帧，按类型分两条队列（与 flv2moq_client 的做法一致：不合并成
     * 一条按 dts 排序的流，各自按文件顺序即可）。 */
    std::deque<flv2moq::FlvFrame> vq, aq;
    /* 还没写出媒体帧、只等配置帧的状态。 */
    bool have_vcfg = false, have_acfg = false;
    std::vector<uint8_t> vcfg, acfg;   /* avcC / ASC（原字节）*/
    std::string vcodec = "avc1.42c00d"; /* 拿不到 avcC 时的兜底串 */
    std::string acodec = "mp4a.40.2";

    std::unique_ptr<flv2moq::FlvReader> reader;

    bool eof = false;
    int64_t anchor_dts = -1;      /* 首帧 dts */
    uint64_t anchor_wall = 0;     /* 首帧时刻的 uv_now */
    int64_t  v_last_us = 0, a_last_us = 0;  /* 上一次写出的微秒时间戳 */
    uint64_t v_frames = 0, a_frames = 0, dropped = 0;
    int64_t  base_dts = -1;       /* 文件第一个 dts，作为时间轴原点 */

    bool done = false;
};

App g;

int64_t NowMs() { return (int64_t)uv_now(g.loop); }

/* 到期时刻 = 锚点墙钟 + (dts - 锚点 dts)/speed；返回「落后了多少毫秒」，
 * >0 = 该发了（或已经晚了），<0 = 还没到点。 */
int64_t Lateness(int64_t dts) {
    if (g.anchor_dts < 0) return 0;
    const int64_t due = (int64_t)g.anchor_wall +
                        (int64_t)((dts - g.anchor_dts) / g.cfg.speed);
    return NowMs() - due;
}

void EnsureAnchor(int64_t dts) {
    if (g.anchor_dts >= 0) return;
    g.anchor_dts = dts;
    g.anchor_wall = uv_now(g.loop);
}

/* ── catalog 的编解码参数从哪来 ──
 * 视频：avcC 就是 FLV 序列头的载荷（flv_reader 已剥掉 5 字节 tag 头）。
 * 音频：ASC 同理（剥掉 2 字节）。codec 串的 profile/level 在 avcC 的第 1、3
 * 字节里 —— 组出 "avc1.PPCCLL" 才能让下游选对解码器。 */
void BuildCodecStrings() {
    if (g.vcfg.size() >= 4) {
        char buf[32];
        /* avcC: [0]=configurationVersion [1]=AVCProfileIndication
         *       [2]=profile_compat [3]=AVCLevelIndication */
        snprintf(buf, sizeof(buf), "avc1.%02x%02x%02x",
                 g.vcfg[1], g.vcfg[2], g.vcfg[3]);
        g.vcodec = buf;
    }
    /* ASC 的 objectTypeIndication 高位 + 采样率/声道索引直接体现在
     * sample_rate/channels 上，codec 串恒为 mp4a.40.2（AAC-LC）。 */
}

/* ── 把帧喂给库 ─────────────────────────────── */

void FeedVideo(const flv2moq::FlvFrame &f) {
    if (g.vt == 0) return;
    /* ts_us：**绝对微秒**。以文件第一个 dts 为原点（对端不关心绝对零点，
     * 只关心差值单调）。 */
    const uint64_t ts_us = (uint64_t)((f.dts - g.base_dts) * 1000);
    g.v_last_us = (int64_t)ts_us;
    if (moq_lite_track_frame(g.vt, ts_us, f.payload.data(), f.payload.size(),
                             f.key ? 1 : 0) == MOQ_LITE_OK)
        g.v_frames++;
}

void FeedAudio(const flv2moq::FlvFrame &f) {
    if (g.at == 0) return;
    const uint64_t ts_us = (uint64_t)((f.dts - g.base_dts) * 1000);
    g.a_last_us = (int64_t)ts_us;
    if (moq_lite_track_frame(g.at, ts_us, f.payload.data(), f.payload.size(),
                             0) == MOQ_LITE_OK)
        g.a_frames++;
}

/* 从 reader 尽量多搬一些到两条队列里。返回 false = 文件读完了。
 *
 * ⚠️ 队列满时要**停下、把帧留在 reader 里**，不能先抽出来再判断收不收 ——
 * 那个「抽出来」就是丢帧。早先的写法是「先 TakeFront，再看容量够不够，不够
 * 就丢弃」，于是一旦队列顶到上限，之后每次 Fill() 读进来的帧**几乎全被丢掉**：
 * 实测推流约 7 秒后队列满，音频组流从每条 ~2.7 KB 掉到只剩组头（14 字节），
 * 视频的关键帧也被丢光（组开了却没有关键帧可解码）。现象就是
 * 「只有一句声音、完全没有画面」。 */
bool Refill() {
    if (!g.reader) return false;
    int guard = 64;
    while (guard-- > 0) {
        while (!g.reader->Empty()) {
            /* 满了就返回，帧留在 reader 队列里等下一轮 —— 不取出、不丢弃。 */
            if (g.vq.size() + g.aq.size() >= kMaxPending) return true;
            flv2moq::FlvFrame f = g.reader->TakeFront();
            if (f.is_seq_hdr) {
                /* 配置帧：**不当作媒体发**，攒下来喂给 catalog。 */
                if (f.is_video) { g.vcfg = std::move(f.payload); g.have_vcfg = true; }
                else            { g.acfg = std::move(f.payload); g.have_acfg = true; }
                continue;
            }
            if (g.base_dts < 0) g.base_dts = f.dts;
            if (f.is_video) g.vq.push_back(std::move(f));
            else            g.aq.push_back(std::move(f));
        }
        if (g.reader->Eof()) { g.eof = true; return !g.vq.empty() || !g.aq.empty(); }
        if (g.vq.size() + g.aq.size() >= kMaxPending) return true;
        if (!g.reader->Fill()) break;
    }
    return true;
}

/* 两条队列各推一帧（一次 tick 最多各一帧，避免一口气灌爆对端）。
 * 落后太多的非关键帧直接丢；关键帧落后太多则**重锚**（丢关键帧会毁掉整个
 * 组的可解码性，比短暂的加速更糟）。 */
void PumpVideo() {
    if (g.vq.empty()) return;
    EnsureAnchor(g.vq.front().dts);
    const int64_t late = Lateness(g.vq.front().dts);
    if (late > kMaxLateMs && !g.vq.front().key) {
        g.vq.pop_front(); g.dropped++;
        return;
    }
    if (g.vq.front().key && late > kResyncLateMs) {
        g.anchor_dts = g.vq.front().dts;
        g.anchor_wall = uv_now(g.loop);
    }
    if (Lateness(g.vq.front().dts) < 0) return;
    FeedVideo(g.vq.front());
    g.vq.pop_front();
}

void PumpAudio() {
    if (g.aq.empty()) return;
    if (Lateness(g.aq.front().dts) > kMaxLateMs) {
        g.aq.pop_front(); g.dropped++;
        return;
    }
    if (Lateness(g.aq.front().dts) < 0) return;
    FeedAudio(g.aq.front());
    g.aq.pop_front();
}

void Tick(uv_timer_t *);

/* 全部发完 / 超过 -t 限制 → 收尾。 */
void Finish() {
    if (g.done) return;
    g.done = true;
    uv_timer_stop(&g.tick);
    if (g.vt) moq_lite_track_end(g.vt);
    if (g.at) moq_lite_track_end(g.at);
    LOG_INFO("[flv2moqlite] done: video=%llu audio=%llu dropped=%llu",
             (unsigned long long)g.v_frames, (unsigned long long)g.a_frames,
             (unsigned long long)g.dropped);
    /* 给库一点时间把收尾的 FIN 发出去，然后收工。 */
    uv_timer_t *bye = (uv_timer_t *)calloc(1, sizeof(uv_timer_t));
    uv_timer_init(g.loop, bye);
    uv_timer_start(bye, [](uv_timer_t *t) {
        uv_close((uv_handle_t *)t, [](uv_handle_t *h) { free(h); });
        uv_stop(g.loop);
    }, 300, 0);
}

void Tick(uv_timer_t *) {
    if (g.done) return;
    Refill();
    PumpVideo();
    PumpAudio();

    if (g.cfg.seconds > 0 && g.anchor_dts >= 0) {
        const int64_t elapsed = (int64_t)((NowMs() - (int64_t)g.anchor_wall) *
                                          g.cfg.speed);
        if (elapsed > (int64_t)g.cfg.seconds * 1000) { Finish(); return; }
    }
    if (g.eof && g.vq.empty() && g.aq.empty()) Finish();
}

/* ── 会话回调 ───────────────────────────────── */

void OnConnected(moq_lite_session_t s, int status, void *user) {
    (void)user;
    if (status != 0) {
        LOG_ERROR("[flv2moqlite] connect failed: %d (%s)", status,
                  moq_lite_strerror(status));
        uv_stop(g.loop);
        return;
    }
    LOG_INFO("[flv2moqlite] connected: alpn=\"%s\"", moq_lite_session_alpn(s));

    /* 先把配置帧读出来 —— 它们决定 catalog 的 description（avcC/ASC）。
     * ⚠️ 必须在声明轨**之前**读：Refill 平时只在 Tick 里跑，而 Tick 要等这里
     * 声明完才起，所以不主动读一次的话这一步永远拿不到序列头（第一次实测就是
     * 卡在这儿：连上了，然后「FLV 里没有序列头」）。 */
    for (int i = 0; i < 8 && !g.have_vcfg && !g.have_acfg && !g.eof; i++) {
        Refill();
    }

    /* 等配置帧到齐再声明轨 —— catalog 里的 description（avcC/ASC）就是
     * 从它们来的，缺了它下游建不出解码器。 */
    if (!g.have_vcfg && !g.have_acfg) {
        LOG_ERROR("[flv2moqlite] no sequence header in the FLV; cannot build "
                  "a catalog");
        uv_stop(g.loop);
        return;
    }

    moq_lite_broadcast_t bc = moq_lite_publish(s, g.cfg.broadcast.c_str());
    if (!bc) {
        LOG_ERROR("[flv2moqlite] publish(%s) failed: %s",
                  g.cfg.broadcast.c_str(), moq_lite_strerror(-1));
        uv_stop(g.loop);
        return;
    }
    BuildCodecStrings();

    if (g.have_vcfg) {
        moq_lite_rendition_t r{};
        r.name = g.cfg.vtrack.c_str();
        r.codec = g.vcodec.c_str();
        r.config = g.vcfg.data();
        r.config_len = g.vcfg.size();
        r.width = 0; r.height = 0;   /* FLV 头里没有尺寸；catalog 允许 0 */
        g.vt = moq_lite_publish_track(bc, /*is_video=*/1, &r);
        if (!g.vt) LOG_ERROR("[flv2moqlite] publish_track(video) failed: %s",
                             moq_lite_strerror(-1));
    }
    if (g.have_acfg) {
        moq_lite_rendition_t r{};
        r.name = g.cfg.atrack.c_str();
        r.codec = g.acodec.c_str();
        r.config = g.acfg.data();
        r.config_len = g.acfg.size();
        /* ASC 里带着采样率索引；这里报常见值。改成从 ASC 解也可以，但 catalog
         * 只用于下游选解码器，0 也是合法的。 */
        r.sample_rate = 48000;
        r.channels = 1;
        g.at = moq_lite_publish_track(bc, /*is_video=*/0, &r);
        if (!g.at) LOG_ERROR("[flv2moqlite] publish_track(audio) failed: %s",
                             moq_lite_strerror(-1));
    }
    g.announced = 1;

    LOG_INFO("[flv2moqlite] publishing \"%s\" (video=%s audio=%s), waiting "
             "for subscribers...", g.cfg.broadcast.c_str(),
             g.vt ? g.cfg.vtrack.c_str() : "-",
             g.at ? g.cfg.atrack.c_str() : "-");

    /* 起节拍器。等对端来订阅期间它空转 —— 帧还在队列里没动。
     * ⚠️ 必须先 uv_timer_init —— 直接 start 一个全零的 uv_timer_t 会踩空
     * （实测 SIGSEGV，而且崩在 uv_run 之前，看起来像「连上就退出了」）。 */
    uv_timer_init(g.loop, &g.tick);
    uv_timer_start(&g.tick, Tick, kTickMs, kTickMs);
}

void OnClose(moq_lite_session_t s, int err, const char *reason, void *user) {
    (void)s; (void)user;
    LOG_INFO("[flv2moqlite] session closed: err=%d (%s)", err,
             reason ? reason : "");
    uv_stop(g.loop);
}

/* ── main ───────────────────────────────────── */

void Usage(const char *argv0) {
    fprintf(stderr,
            "usage: %s <flv-file> <moql://host:port/path> <broadcast>\n"
            "          [-s <speed>] [-t <seconds>] [--vtrack name] [--atrack name]\n"
            "\n"
            "  file       FLV file (H.264 + AAC)\n"
            "  url        server address; use the DOMAIN, not an IP\n"
            "             (the server cert has no IP SAN)\n"
            "  broadcast  on-wire broadcast name, e.g. cli_test.hang\n"
            "\n"
            "example: %s test.flv moql://www.webrtcserver.com.cn:4433/anon \\\n"
            "             cli_test.hang\n",
            argv0, argv0);
}

} /* namespace */

int main(int argc, char *argv[]) {
    setbuf(stdout, nullptr);
    if (argc < 4) { Usage(argv[0]); return 2; }

    g.cfg.flv_path  = argv[1];
    g.cfg.url       = argv[2];
    g.cfg.broadcast = argv[3];
    for (int i = 4; i < argc; i++) {
        if (!strcmp(argv[i], "-s") && i + 1 < argc) g.cfg.speed = atof(argv[++i]);
        else if (!strcmp(argv[i], "-t") && i + 1 < argc) g.cfg.seconds = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--vtrack") && i + 1 < argc) g.cfg.vtrack = argv[++i];
        else if (!strcmp(argv[i], "--atrack") && i + 1 < argc) g.cfg.atrack = argv[++i];
        else { Usage(argv[0]); return 2; }
    }
    if (g.cfg.speed <= 0.0) { LOG_ERROR("speed must be > 0"); return 2; }

    log_set_level(INFO);
    log_set_file("flv2moqlite_client.log");

    /* 先确认是 FLV —— 免得白连一次再失败。 */
    std::string err;
    if (!flv2moq::CheckFlvMagic(g.cfg.flv_path, err)) {
        LOG_ERROR("[flv2moqlite] %s: %s", g.cfg.flv_path.c_str(), err.c_str());
        return 1;
    }
    g.reader.reset(new flv2moq::FlvReader());
    if (g.reader->Open(g.cfg.flv_path) != 0) return 1;

    /* 循环归**本程序**：库只是挂在它上面的一个用户（库不拥有循环，见
     * moq_lite_session_new 的说明）。 */
    g.loop = uv_default_loop();

    moq_lite_session_cb_t cb{};
    cb.on_connected = OnConnected;
    cb.on_close     = OnClose;
    g.sess = moq_lite_session_new(g.loop);
    if (!g.sess) { LOG_ERROR("session_new failed"); return 1; }

    /* 推流方向：Role=PUBLISHER，对端才会来订阅我们。 */
    int src = moq_lite_session_set_role(g.sess, MOQ_LITE_ROLE_PUBLISHER);
    if (src != MOQ_LITE_OK) {
        LOG_ERROR("set_role failed: %s", moq_lite_strerror(src));
        return 1;
    }
    int rc = moq_lite_session_connect(g.sess, g.cfg.url.c_str(), &cb);
    if (rc != MOQ_LITE_OK) {
        LOG_ERROR("connect failed: %s", moq_lite_strerror(rc));
        return 1;
    }

    LOG_INFO("[flv2moqlite] %s -> %s broadcast=%s speed=%.2f",
             g.cfg.flv_path.c_str(), g.cfg.url.c_str(), g.cfg.broadcast.c_str(),
             g.cfg.speed);

    uv_run(g.loop, UV_RUN_DEFAULT);
    return 0;
}
