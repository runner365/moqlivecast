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
