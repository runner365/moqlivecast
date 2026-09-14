#include "logger.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <stdarg.h>
#include <pthread.h>

/* ============================================
 * 日志队列节点
 * ============================================ */
typedef struct LogNode {
    char          *line;
    struct LogNode *next;
} LogNode;

/* ============================================
 * 全局状态
 * ============================================ */
static FILE           *g_log_file = NULL;
static int             g_console  = 1;
static LogLevel        g_level    = INFO;

static pthread_t       g_thread;
static pthread_mutex_t g_mutex   = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_cond    = PTHREAD_COND_INITIALIZER;
static LogNode        *g_head    = NULL;
static LogNode        *g_tail    = NULL;
static int             g_running = 0;   /* 0=未启动, 1=运行中, -1=退出中 */

static const char *level_names[] = { "ERROR", "WARN ", "INFO ", "DEBUG" };

/* ============================================
 * 日志线程
 * ============================================ */
static void *log_thread(void *arg) {
    (void)arg;

    for (;;) {
        pthread_mutex_lock(&g_mutex);

        while (g_head == NULL && g_running > 0) {
            pthread_cond_wait(&g_cond, &g_mutex);
        }

        /* 退出信号 + 队列已清空 */
        if (g_head == NULL) {
            pthread_mutex_unlock(&g_mutex);
            break;
        }

        LogNode *node = g_head;
        g_head = node->next;
        if (!g_head) g_tail = NULL;
        pthread_mutex_unlock(&g_mutex);

        /* 无锁 IO */
        if (g_log_file) {
            fputs(node->line, g_log_file);
            fflush(g_log_file);
        }
        if (g_console) {
            /* 根据行内容判断是否 stderr（ERROR/WARN 以 E 或 W 开头） */
            int is_err = (node->line[0] == 'E' || node->line[0] == 'W');
            fputs(node->line, is_err ? stderr : stdout);
            fflush(is_err ? stderr : stdout);
        }

        free(node->line);
        free(node);
    }

    return NULL;
}

/* ============================================
 * 启动线程（惰性）
 * ============================================ */
static void ensure_thread(void) {
    if (g_running == 0) {
        g_running = 1;
        pthread_create(&g_thread, NULL, log_thread, NULL);
    }
}

/* ============================================
 * 公共 — 配置
 * ============================================ */
void log_set_file(const char *filename) {
    pthread_mutex_lock(&g_mutex);
    if (g_log_file) {
        fclose(g_log_file);
        g_log_file = NULL;
    }
    if (filename) {
        g_log_file = fopen(filename, "a");
    }
    pthread_mutex_unlock(&g_mutex);
}

void log_set_console(int enable) {
    g_console = enable;
}

void log_set_level(LogLevel level) {
    g_level = level;
}

void log_shutdown(void) {
    pthread_mutex_lock(&g_mutex);
    if (g_running != 1) {
        pthread_mutex_unlock(&g_mutex);
        return;
    }
    g_running = -1;
    pthread_cond_signal(&g_cond);
    pthread_mutex_unlock(&g_mutex);

    pthread_join(g_thread, NULL);

    if (g_log_file) {
        fclose(g_log_file);
        g_log_file = NULL;
    }
}

/* ============================================
 * 公共 — 写日志
 * ============================================ */
void log_write(LogLevel level, const char *file, int line,
               const char *fmt, ...) {
    if (level > g_level) return;
    if (!g_log_file && !g_console) return;

    /* 时间戳 */
    struct timeval tv;
    gettimeofday(&tv, NULL);
    struct tm tm_buf;
    localtime_r(&tv.tv_sec, &tm_buf);

    /* 提取文件名 */
    const char *fname = strrchr(file, '/');
    fname = fname ? fname + 1 : file;

    /* 格式化用户消息 */
    char msg[4096];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    /* 拼装完整行 — 文件输出用 \r\n，控制台在输出时替换 */
    size_t line_len = strlen(fname) + 16 + 32 + 8 + strlen(msg) + 8;
    char *full = (char*)malloc(line_len);
    if (!full) return;

    int written = snprintf(full, line_len,
                           "[%s:%d][%04d-%02d-%02d %02d:%02d:%02d.%03d][%s] %s\r\n",
                           fname, line,
                           tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday,
                           tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec,
                           (int)(tv.tv_usec / 1000),
                           level_names[level], msg);
    if (written < 0) { free(full); return; }

    LogNode *node = (LogNode*)malloc(sizeof(LogNode));
    if (!node) { free(full); return; }
    node->line = full;
    node->next = NULL;

    pthread_mutex_lock(&g_mutex);
    ensure_thread();
    if (g_tail) {
        g_tail->next = node;
        g_tail = node;
    } else {
        g_head = g_tail = node;
    }
    pthread_cond_signal(&g_cond);
    pthread_mutex_unlock(&g_mutex);
}
