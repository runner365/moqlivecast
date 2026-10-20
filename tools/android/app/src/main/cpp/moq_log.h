#ifndef MOQ_LOG_H
#define MOQ_LOG_H

/* ============================================================
 * native 侧的日志出口 —— 统一走 **Logcat**。
 *
 * ⚠️ 在 Android 上，native 的 stdout/stderr 被接到 /dev/null，
 * `printf` / 库里那个写 stdout 的 logger 在 Logcat 里**一个字都看不到**。
 * 所以本目录自己的日志一律用 __android_log_print。
 *
 * 非 Android（宿主机做语法检查）时退回 fprintf，好让这些 .c 单独编得过。
 *
 * 注意分工：**moqlite 库自己的 LOG_INFO 走 logger.c → 落文件**
 * （JNI_OnLoad 里 log_set_file 到 files/moq_lite.log），
 * 本目录的日志走 Logcat。两边分开是有意的：库的日志量大且是英文，
 * 落文件便于全量回看；这里要的是随手 adb logcat 就能看见。
 * ============================================================ */

#ifdef __ANDROID__
#include <android/log.h>
#include <stdio.h>
#define MOQ_LOG_TAG "moqpusher"
/* 同时送给 Kotlin（由 moq_jni.c 实现，内部做 JNI 回调；没开转发时是空操作）。
 * 日志照旧进 Logcat，两条路互不影响。 */
void moq_log_to_kt(int level, const char *msg);
#define MOQ_LOG_KT(lv, ...) do {                     \
        char _ktbuf[512];                            \
        snprintf(_ktbuf, sizeof(_ktbuf), __VA_ARGS__); \
        moq_log_to_kt(lv, _ktbuf);                   \
    } while (0)
#define LOGI(...) do { __android_log_print(ANDROID_LOG_INFO,  MOQ_LOG_TAG, __VA_ARGS__); \
                       MOQ_LOG_KT(2, __VA_ARGS__); } while (0)
#define LOGW(...) do { __android_log_print(ANDROID_LOG_WARN,  MOQ_LOG_TAG, __VA_ARGS__); \
                       MOQ_LOG_KT(1, __VA_ARGS__); } while (0)
#define LOGE(...) do { __android_log_print(ANDROID_LOG_ERROR, MOQ_LOG_TAG, __VA_ARGS__); \
                       MOQ_LOG_KT(0, __VA_ARGS__); } while (0)
#else
#include <stdio.h>
#define LOGI(...) do { fprintf(stderr, "[I] " __VA_ARGS__); fputc('\n', stderr); } while (0)
#define LOGW(...) do { fprintf(stderr, "[W] " __VA_ARGS__); fputc('\n', stderr); } while (0)
#define LOGE(...) do { fprintf(stderr, "[E] " __VA_ARGS__); fputc('\n', stderr); } while (0)
#endif

#endif /* MOQ_LOG_H */
