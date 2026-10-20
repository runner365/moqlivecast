/* ============================================================
 * JNI 桥 —— Kotlin 侧进入 moq-lite C API 的唯一入口。
 *
 * 分工：**Kotlin 只管取数据**（Camera2 / AudioRecord / MediaCodec），
 * **C 这边管协议与比特流**。所以这里的 kotlin 接口是「给我 avcC」「给我一帧
 * Annex-B」「给我一帧 AAC」，而不是让上层去碰 moq-lite 的任何类型。
 *
 * ⚠️ Annex-B → AVCC 的转换放在**这里**、不放 Kotlin：
 *   ① 它是比特流操作，C 写更自然；
 *   ② 放 Kotlin 就要再拷一次 byte[] 过来，白多一趟；
 *   ③ 长度前缀的宽度必须和 avcC 里的 lengthSizeMinusOne 对上（都是 4），
 *      两者放在同一个文件里才不容易走散。
 * ============================================================ */

#include <jni.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "moq_lite_client.h"
#include "moq_pusher.h"
#include "annexb_avcc.h"
#include "moq_log.h"
#include "logger.h"

/* 包名/类名变了这里就全部失效 —— 必须与 app/build.gradle.kts 的 namespace 和
 * MoqNative.kt 的包名逐字一致。对不上时 JVM 抛
 * UnsatisfiedLinkError: No implementation found for ...，很直白。 */

/* ============================================================
 * 原生日志 → Kotlin
 *
 * 两条来源、一个出口：
 *   ① 本目录自己的日志（LOGx → moq_log_to_kt，见 moq_log.h）
 *   ② moqlite 库自己的日志（logger.c 的 sink，在 JNI_OnLoad 里挂上）
 * 两者都汇到 jni_log_emit()，它回调 Kotlin 的
 * MoqNative.emitNativeLog(String)。
 *
 * ⚠️ 这套东西必须能在**任意 native 线程**上调用（loop 线程、日志线程、
 * 编解码回调线程）。没 attach 过 JVM 的线程要现场 AttachCurrentThread，
 * 用完再 Detach —— 否则 ART 会因为线程不退出而报错，且线程本地引用会泄漏。
 * ============================================================ */

static JavaVM   *g_jvm        = NULL;
static jclass    g_log_cls    = NULL;   /* 全局引用：com/example/moqlitepush/MoqNative */
static jmethodID g_log_emit   = NULL;   /* static void emitNativeLog(String) */
static volatile int g_log_on  = 0;      /* Kotlin 侧说要不要收 */

/* 真正过 JNI 的那一步。任何 native 线程可调。 */
static void jni_log_emit(int level, const char *line)
{
    (void)level;   /* 等级已经在文本里（库的行自带 [INFO ]，本目录的由 Kotlin 加） */
    if (!g_log_on || !g_jvm || !g_log_cls || !g_log_emit || !line) return;

    JNIEnv *env = NULL;
    int attached = 0;
    if ((*g_jvm)->GetEnv(g_jvm, (void **)&env, JNI_VERSION_1_6) != JNI_OK) {
        if ((*g_jvm)->AttachCurrentThread(g_jvm, &env, NULL) != JNI_OK) return;
        attached = 1;
    }

    /* ⚠️ NewStringUTF 收的是 **modified UTF-8**。日志串按约定是纯英文
     * （见 CLAUDE.md：运行期日志一律英文），所以这里不需要转换。 */
    jstring s = (*env)->NewStringUTF(env, line);
    if (s) {
        (*env)->CallStaticVoidMethod(env, g_log_cls, g_log_emit, s);
        (*env)->DeleteLocalRef(env, s);
    }
    /* Kotlin 侧抛异常不能顺着 native 栈往上爬 —— 就地清掉，否则后续 JNI
     * 调用全部失败（“pending exception”）。 */
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);

    if (attached) (*g_jvm)->DetachCurrentThread(g_jvm);
}

/* 本目录 LOGx 的出口（moq_log.h 里声明）。 */
void moq_log_to_kt(int level, const char *msg)
{
    jni_log_emit(level, msg);
}

/* moqlite 库 logger.c 的 sink：在库的日志线程上逐行回调。
 * 行以 "\r\n" 结尾，这里裁掉 —— Kotlin 侧按行显示。 */
static void lib_log_sink(int level, const char *line, void *user)
{
    (void)user;
    if (!g_log_on || !line) return;

    char buf[1024];
    size_t n = strlen(line);
    while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) n--;
    if (n > sizeof(buf) - 1) n = sizeof(buf) - 1;
    memcpy(buf, line, n);
    buf[n] = '\0';
    jni_log_emit(level, buf);
}

/* .so 被 System.loadLibrary 加载时调一次。
 * 把进程级的东西（互斥量、信号量）在这里建好，之后所有入口都安全 ——
 * 包括 UI 在 start 之前就开始轮询的 nativeGetStats。 */
JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void *reserved)
{
    (void)reserved;
    g_jvm = vm;                 /* 日志回调要跨线程用它 attach */
    if (moq_pusher_global_init() != 0) return JNI_ERR;
    log_set_sink(lib_log_sink, NULL);   /* 库的日志也并到这个出口 */
    return JNI_VERSION_1_6;
}

/* ── 诊断 ─────────────────────────────────────── */

JNIEXPORT jstring JNICALL
Java_com_example_moqlitepush_MoqNative_version(JNIEnv *env, jobject thiz)
{
    (void)thiz;
    return (*env)->NewStringUTF(env, MOQ_LITE_VERSION);
}

/* 自检：不碰网络，只证明 libuv + OpenSSL + moq-lite 在 Bionic 上构造得出来。
 * 0 = 通过，<0 = 失败码。 */
JNIEXPORT jint JNICALL
Java_com_example_moqlitepush_MoqNative_selfTest(JNIEnv *env, jobject thiz)
{
    (void)env; (void)thiz;

    /* ⚠️ 必须先做全局初始化 —— 它会把 `UV_USE_IO_URING` 关掉。
     * 不关的话，**任何**建 loop 的调用（uv_default_loop() / uv_loop_init()）都会
     * 让 libuv 去调 io_uring 的 syscall，而 Android 的 seccomp 不允许，进程直接
     * 收信号死掉 —— 栈回溯停在 `syscall(__NR_io_uring_setup, …)`。
     * 详见 moq_pusher.c 里 moq_pusher_global_init() 的长注释。
     * 正常路径上 JNI_OnLoad 已经调过一次，这里是幂等的兜底。 */
    if (moq_pusher_global_init() != 0) return -1;

    uv_loop_t *loop = uv_default_loop();
    if (!loop) return -1;

    moq_lite_session_t s = moq_lite_session_new(loop);
    if (!s) return -2;
    if (moq_lite_session_set_role(s, MOQ_LITE_ROLE_PUBLISHER) != MOQ_LITE_OK) {
        moq_lite_session_close(s);
        return -3;
    }
    moq_lite_session_close(s);
    return 0;
}

/* 开/关原生日志转发。开着才回调 Kotlin 的 MoqNative.emitNativeLog(String)；
 * 关掉时 jni_log_emit 第一行就返回 —— 一个字节都不拷、一次 JNI 都不过。
 *
 * 句柄在**第一次打开时**缓存（GlobalRef + 方法 ID）：之后 native 线程回调
 * 不必再去找类。方法 ID 在进程内稳定，缓存是安全的。 */
JNIEXPORT void JNICALL
Java_com_example_moqlitepush_MoqNative_nativeSetLogForwarding(JNIEnv *env, jobject thiz,
                                                              jboolean on)
{
    if (!on) { g_log_on = 0; return; }

    if (!g_log_cls) {
        jclass local = (*env)->GetObjectClass(env, thiz);
        if (!local) return;
        g_log_cls = (jclass)(*env)->NewGlobalRef(env, local);
        (*env)->DeleteLocalRef(env, local);
        if (!g_log_cls) return;
    }
    if (!g_log_emit) {
        /* @JvmStatic → 是个**静态**方法，所以用 GetStaticMethodID。 */
        g_log_emit = (*env)->GetStaticMethodID(env, g_log_cls, "emitNativeLog",
                                               "(Ljava/lang/String;)V");
        if (!g_log_emit) {
            if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
            return;
        }
    }
    g_log_on = 1;
}

/* ── 推流生命周期 ─────────────────────────────── */

/* start(url, broadcast, logPath)
 *
 * logPath 非空时把**库自己**的日志写进那个文件。这一步在 Android 上必须做：
 * native 的 stdout/stderr 接到 /dev/null，库里的 LOG_INFO 在 Logcat 里一个字
 * 都看不到；写成文件才能 adb pull 出来看。
 * 本文件自己的日志走 Logcat（见 moq_log.h）。 */
JNIEXPORT jint JNICALL
Java_com_example_moqlitepush_MoqNative_nativeStart(JNIEnv *env, jobject thiz,
                                                   jstring url, jstring broadcast,
                                                   jstring logPath)
{
    (void)thiz;
    if (!url) return -1;

    if (logPath) {
        const char *lp = (*env)->GetStringUTFChars(env, logPath, NULL);
        if (lp) {
            log_set_console(0);      /* stdout 在 Android 上没人看，别写了 */
            log_set_file(lp);
            (*env)->ReleaseStringUTFChars(env, logPath, lp);
        }
    }

    const char *u = (*env)->GetStringUTFChars(env, url, NULL);
    if (!u) return -1;

    const char *b = broadcast ? (*env)->GetStringUTFChars(env, broadcast, NULL) : NULL;

    int rc = moq_pusher_start(u, b);

    if (b) (*env)->ReleaseStringUTFChars(env, broadcast, b);
    (*env)->ReleaseStringUTFChars(env, url, u);
    return rc;
}

JNIEXPORT void JNICALL
Java_com_example_moqlitepush_MoqNative_nativeStop(JNIEnv *env, jobject thiz)
{
    (void)env; (void)thiz;
    moq_pusher_stop();
}

/* 收尾但不断连接：给每条订阅的当前组补结束哨兵 + FIN。 */
JNIEXPORT void JNICALL
Java_com_example_moqlitepush_MoqNative_nativeFinishTracks(JNIEnv *env, jobject thiz)
{
    (void)env; (void)thiz;
    moq_pusher_finish_tracks();
}

/* ── 编解码参数 ───────────────────────────────── */

/* 视频：avcc 是**已经拼好的 avcC 记录**（Kotlin 侧用 SPS/PPS 拼，或直接用
 * 本文件的 nativeBuildAvcC）。 */
JNIEXPORT jint JNICALL
Java_com_example_moqlitepush_MoqNative_nativeVideoConfig(JNIEnv *env, jobject thiz,
                                                         jbyteArray avcc, jint width,
                                                         jint height, jstring track)
{
    (void)thiz;
    if (!avcc) return -1;

    jsize n = (*env)->GetArrayLength(env, avcc);
    if (n <= 0) return -1;
    jbyte *p = (*env)->GetByteArrayElements(env, avcc, NULL);
    if (!p) return -1;

    const char *t = track ? (*env)->GetStringUTFChars(env, track, NULL) : NULL;
    int rc = moq_pusher_set_video_config((const uint8_t *)p, (size_t)n,
                                        (uint32_t)width, (uint32_t)height, t);
    if (t) (*env)->ReleaseStringUTFChars(env, track, t);
    (*env)->ReleaseByteArrayElements(env, avcc, p, JNI_ABORT);   /* 只读，别回写 */
    return rc;
}

/* 音频：asc 就是 MediaCodec 的 csd-0（AudioSpecificConfig 原字节）。 */
JNIEXPORT jint JNICALL
Java_com_example_moqlitepush_MoqNative_nativeAudioConfig(JNIEnv *env, jobject thiz,
                                                         jbyteArray asc, jint sampleRate,
                                                         jint channels, jstring track)
{
    (void)thiz;
    if (!asc) return -1;

    jsize n = (*env)->GetArrayLength(env, asc);
    if (n <= 0) return -1;
    jbyte *p = (*env)->GetByteArrayElements(env, asc, NULL);
    if (!p) return -1;

    const char *t = track ? (*env)->GetStringUTFChars(env, track, NULL) : NULL;
    int rc = moq_pusher_set_audio_config((const uint8_t *)p, (size_t)n,
                                        (uint32_t)sampleRate, (uint32_t)channels, t);
    if (t) (*env)->ReleaseStringUTFChars(env, track, t);
    (*env)->ReleaseByteArrayElements(env, asc, p, JNI_ABORT);
    return rc;
}

/* 由 MediaCodec 的 csd-0/csd-1 拼 avcC。上层拿到 avcC 之后连同尺寸一起交给
 * nativeVideoConfig —— 拆两步是为了让上层能自己看一眼 avcC（排错时有用）。 */
JNIEXPORT jbyteArray JNICALL
Java_com_example_moqlitepush_MoqNative_nativeBuildAvcC(JNIEnv *env, jobject thiz,
                                                       jbyteArray csd0, jbyteArray csd1)
{
    (void)thiz;
    if (!csd0) return NULL;

    jsize n0 = (*env)->GetArrayLength(env, csd0);
    jbyte *p0 = (*env)->GetByteArrayElements(env, csd0, NULL);
    if (!p0) return NULL;

    jsize n1 = 0; jbyte *p1 = NULL;
    if (csd1) {
        n1 = (*env)->GetArrayLength(env, csd1);
        if (n1 > 0) p1 = (*env)->GetByteArrayElements(env, csd1, NULL);
    }

    uint8_t out[128];
    int r = build_avcc_from_csd((const uint8_t *)p0, (size_t)n0,
                               (const uint8_t *)p1, (size_t)n1,
                               out, sizeof(out));

    if (p1) (*env)->ReleaseByteArrayElements(env, csd1, p1, JNI_ABORT);
    (*env)->ReleaseByteArrayElements(env, csd0, p0, JNI_ABORT);
    if (r <= 0) return NULL;

    jbyteArray res = (*env)->NewByteArray(env, r);
    if (res) (*env)->SetByteArrayRegion(env, res, 0, r, (const jbyte *)out);
    return res;
}

/* ── 帧 ───────────────────────────────────────── */

/* 视频一帧。annexb 是 MediaCodec 直接吐出来的 Annex-B，这里转成 AVCC 再入队。
 *
 * `len` 显式传：上层复用同一个 byte[] 接 MediaCodec 的输出（省掉每帧一次分配），
 * 所以数组实际长度通常大于这一帧的字节数 —— 按 GetArrayLength 取就会把上一次的
 * 残留一起发出去。
 *
 * ⚠️ 上层必须在 `releaseOutputBuffer` **之前**调它 —— 传进来的是 MediaCodec
 * 输出 ByteBuffer 的内容，buffer 交还之后就读不到了。（本函数内部会拷贝，
 * 所以返回之后上层就能安全 release。） */
JNIEXPORT jint JNICALL
Java_com_example_moqlitepush_MoqNative_nativeSendVideo(JNIEnv *env, jobject thiz,
                                                       jlong tsUs, jbyteArray annexb,
                                                       jint len, jboolean key)
{
    (void)thiz;
    if (!annexb || len <= 0) return -1;

    jsize cap_arr = (*env)->GetArrayLength(env, annexb);
    if (len > cap_arr) return -1;            /* 防越界：上层传错了 */
    const size_t n = (size_t)len;

    jbyte *p = (*env)->GetByteArrayElements(env, annexb, NULL);
    if (!p) return -1;

    /* dst 容量：3 字节起始码换成 4 字节长度前缀，每个 NALU 最多多 1 字节；
     * NALU 至少 1 字节 + 3 字节起始码 ⇒ 个数 ≤ n/4。留足余量。 */
    size_t cap = n + n / 3 + 64;
    uint8_t *dst = (uint8_t *)malloc(cap);
    if (!dst) {
        (*env)->ReleaseByteArrayElements(env, annexb, p, JNI_ABORT);
        return -1;
    }

    int r = annexb_to_avcc((const uint8_t *)p, n, dst, cap);
    if (r > 0) {
        moq_pusher_send_video((uint64_t)tsUs, dst, (size_t)r, key ? 1 : 0);
    } else {
        /* ⚠️ 这条以前是**完全静默**的（调用方也不看返回值）—— 症状就是
         * 「推流端一切正常、播放端没有画面」，两边都查不出东西。
         * Annex-B 必须以 00 00 01 / 00 00 00 01 开头；不是的话说明这个编码器
         * 吐的是别的形状（比如已经是 AVCC），那就得换转换方式而不是丢弃。 */
        static int warned = 0;
        if (warned < 5) {
            warned++;
            LOGW("video frame is NOT Annex-B (no start code): len=%d "
                 "head=%02x%02x%02x%02x %02x%02x%02x%02x",
                 (int)n,
                 n > 0 ? p[0] : 0, n > 1 ? p[1] : 0, n > 2 ? p[2] : 0, n > 3 ? p[3] : 0,
                 n > 4 ? p[4] : 0, n > 5 ? p[5] : 0, n > 6 ? p[6] : 0, n > 7 ? p[7] : 0);
        }
    }

    free(dst);
    (*env)->ReleaseByteArrayElements(env, annexb, p, JNI_ABORT);
    return (r > 0) ? 0 : -1;
}

/* 音频一帧。MediaCodec 的 AAC 输出就是**裸 AAC**（没有 ADTS 头），正是要的。
 * `len` 的道理同 nativeSendVideo。 */
JNIEXPORT jint JNICALL
Java_com_example_moqlitepush_MoqNative_nativeSendAudio(JNIEnv *env, jobject thiz,
                                                       jlong tsUs, jbyteArray aac,
                                                       jint len)
{
    (void)thiz;
    if (!aac || len <= 0) return -1;

    jsize cap_arr = (*env)->GetArrayLength(env, aac);
    if (len > cap_arr) return -1;

    jbyte *p = (*env)->GetByteArrayElements(env, aac, NULL);
    if (!p) return -1;

    moq_pusher_send_audio((uint64_t)tsUs, (const uint8_t *)p, (size_t)len);
    (*env)->ReleaseByteArrayElements(env, aac, p, JNI_ABORT);
    return 0;
}

/* ── 统计 ─────────────────────────────────────── */

/* 填一个长度 >= 6 的 long[]：
 *   [0] running  [1] connected  [2] published
 *   [3] video_frames  [4] audio_frames  [5] dropped                         */
JNIEXPORT jint JNICALL
Java_com_example_moqlitepush_MoqNative_nativeGetStats(JNIEnv *env, jobject thiz,
                                                      jlongArray out)
{
    (void)thiz;
    if (!out || (*env)->GetArrayLength(env, out) < 6) return -1;

    moq_pusher_stats_t s;
    memset(&s, 0, sizeof(s));
    moq_pusher_get_stats(&s);

    jlong v[6];
    v[0] = s.running;
    v[1] = s.connected;
    v[2] = s.published;
    v[3] = (jlong)s.video_frames;
    v[4] = (jlong)s.audio_frames;
    v[5] = (jlong)s.dropped;

    (*env)->SetLongArrayRegion(env, out, 0, 6, v);
    return 0;
}
