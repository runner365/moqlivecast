#ifndef LOGGER_H
#define LOGGER_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ERROR = 0,
    WARN  = 1,
    INFO  = 2,
    DEBUG = 3
} LogLevel;

void log_set_file(const char *filename);
void log_set_console(int enable);
void log_set_level(LogLevel level);
void log_shutdown(void);

/* 额外的日志出口 —— 每一行（已成型、带文件名/时间戳）**在日志线程上**回调一次。
 * Android 上就是用它把库自己的日志经 JNI 转给 Kotlin（见 moq_jni.c）。
 *
 * ⚠️ 两个约定：
 *   ① 回调在**日志线程**上跑，不是调用 log_write 的那条线程 —— 里面别做重活，
 *      更不能反过来再写日志（会绕回来）。
 *   ② `line` 以 "\r\n" 结尾，且只在本次回调期间有效；要留就自己拷一份。
 * 出口彼此独立：file / console / sink 关掉哪个都不影响另外两个。 */
typedef void (*log_sink_fn)(int level, const char *line, void *user);
void log_set_sink(log_sink_fn fn, void *user);

void log_write(LogLevel level, const char *file, int line,
               const char *fmt, ...);

#define LOG_ERROR(fmt, ...) log_write(ERROR, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define LOG_WARN(fmt, ...)  log_write(WARN,  __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define LOG_INFO(fmt, ...)  log_write(INFO,  __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define LOG_DEBUG(fmt, ...) log_write(DEBUG, __FILE__, __LINE__, fmt, ##__VA_ARGS__)

#ifdef __cplusplus
}
#endif

#endif
