#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <uv.h>

/* ============================================
 * 可调参数（命令行 --loss XX --delay-min XX --delay-max XX --reorder N）
 * ============================================ */
static double g_loss_rate   = 0.10;  /* 10% 丢包率 */
static int    g_delay_min   = 5;     /* 最小延迟 ms */
static int    g_delay_max   = 50;    /* 最大延迟 ms */
static int    g_reorder_n   = 3;     /* 攒 N 个包随机换序，0=不换序 */
static int    g_port_face   = 3443;
static int    g_port_back   = 3444;
static const char *g_server_ip   = "127.0.0.1";
static int         g_server_port = 4433;

/* 延迟队列节点 — 单链表 */
typedef struct delay_entry {
    struct delay_entry *next;
    uint8_t            *data;
    size_t              len;
    struct sockaddr_in  dst;
    uint64_t            fire_at_ms;
} delay_entry;

static delay_entry *g_delay_head = NULL;

/* 乱序缓冲 — 固定大小环形 buffer */
typedef struct {
    delay_entry *entries;   /* 指针数组 */
    int cap;
    int cnt;
} reorder_buf;

/* ============================================
 * 全局状态
 * ============================================ */

static uv_loop_t *g_loop;
static uv_udp_t  g_face;   /* :3443 面向 Chrome */
static uv_udp_t  g_back;   /* :3444 面向 Server */
static uv_timer_t g_drain_timer;

static struct sockaddr_in g_chrome_addr;  /* Chrome 地址（首个收包时记录） */
static int                g_chrome_bound = 0;

/* 配置传递 — uv_timer.data 指向此结构 */
static struct {
    uv_udp_t *from;
    struct sockaddr_in *from_addr;
    uv_udp_t *to;
    struct sockaddr_in *to_addr;
    reorder_buf *rbuf;
} g_dir[2]; /* 0=Chrome→Server, 1=Server→Chrome */

/* ============================================
 * 乱序缓冲
 * ============================================ */

static reorder_buf* reorder_create(int cap) {
    reorder_buf *rb = (reorder_buf*)calloc(1, sizeof(*rb));
    rb->entries = (delay_entry*)calloc((size_t)cap, sizeof(delay_entry));
    rb->cap = cap;
    rb->cnt = 0;
    return rb;
}

static void reorder_push(reorder_buf *rb, const uint8_t *data, size_t len,
                          struct sockaddr_in *dst, uint64_t fire_at) {
    if (rb->cnt >= rb->cap) {
        /* 缓冲区满: 最旧条目移交所有权后入 delay 队列 */
        delay_entry *old = &rb->entries[0];
        delay_entry *ne = (delay_entry*)calloc(1, sizeof(*ne));
        ne->data       = old->data;
        ne->len        = old->len;
        ne->dst        = old->dst;
        ne->fire_at_ms = old->fire_at_ms;
        ne->next       = g_delay_head;
        g_delay_head   = ne;
        memmove(&rb->entries[0], &rb->entries[1],
                (size_t)(rb->cap - 1) * sizeof(delay_entry));
        rb->cnt--;
    }
    int idx = rb->cnt++;
    delay_entry *e = &rb->entries[idx];
    e->data = (uint8_t*)malloc(len);
    memcpy(e->data, data, len);
    e->len  = len;
    e->dst  = *dst;
    e->fire_at_ms = fire_at;
    e->next = NULL;
}

/* 随机打乱后全部丢到 delay 队列 */
static void reorder_flush(reorder_buf *rb) {
    int i, j;
    /* Fisher-Yates shuffle */
    for (i = rb->cnt - 1; i > 0; i--) {
        j = rand() % (i + 1);
        delay_entry tmp = rb->entries[i];
        rb->entries[i] = rb->entries[j];
        rb->entries[j] = tmp;
    }
    /* 重新分配入队（entries 是就地数组，需独立分配才能链入 delay 队列） */
    for (i = 0; i < rb->cnt; i++) {
        delay_entry *ne = (delay_entry*)calloc(1, sizeof(*ne));
        ne->data       = rb->entries[i].data;
        ne->len        = rb->entries[i].len;
        ne->dst        = rb->entries[i].dst;
        ne->fire_at_ms = rb->entries[i].fire_at_ms;
        ne->next       = g_delay_head;
        g_delay_head   = ne;
        rb->entries[i].data = NULL;  /* 移交所有权 */
    }
    rb->cnt = 0;
}

static void reorder_free(reorder_buf *rb) {
    int i;
    for (i = 0; i < rb->cnt; i++) free(rb->entries[i].data);
    free(rb->entries);
    free(rb);
}

/* ============================================
 * 延迟队列 — 按 fire_at_ms 升序插入
 * ============================================ */

static void delay_enqueue(const uint8_t *data, size_t len,
                           struct sockaddr_in *dst, uint64_t fire_at_ms) {
    delay_entry *ne = (delay_entry*)calloc(1, sizeof(*ne));
    ne->data = (uint8_t*)malloc(len);
    memcpy(ne->data, data, len);
    ne->len  = len;
    ne->dst  = *dst;
    ne->fire_at_ms = fire_at_ms;

    /* 降序插入（fire_at_ms 小的在前） */
    if (!g_delay_head || g_delay_head->fire_at_ms > ne->fire_at_ms) {
        ne->next = g_delay_head;
        g_delay_head = ne;
    } else {
        delay_entry *cur = g_delay_head;
        while (cur->next && cur->next->fire_at_ms <= ne->fire_at_ms)
            cur = cur->next;
        ne->next = cur->next;
        cur->next = ne;
    }
}

/* ============================================
 * 出队 + 发送
 * ============================================ */

static void delay_drain(uv_timer_t *timer) {
    (void)timer;
    uint64_t now = uv_now(g_loop);

    while (g_delay_head && g_delay_head->fire_at_ms <= now) {
        delay_entry *e = g_delay_head;
        g_delay_head = e->next;

        /* 发送到目标 */
        uv_buf_t buf = uv_buf_init((char*)e->data, (unsigned int)e->len);
        int r;
        /* 判断目标端口 */
        if (e->dst.sin_port == htons((uint16_t)g_server_port)) {
            r = uv_udp_try_send(&g_back, &buf, 1,
                                (const struct sockaddr*)&e->dst);
        } else {
            r = uv_udp_try_send(&g_face, &buf, 1,
                                (const struct sockaddr*)&e->dst);
        }
        if (r < 0) {
            fprintf(stderr, "[proxy] drain send failed: %s\n",
                    uv_strerror(r));
        }

        free(e->data);
        free(e);
    }
}

/* ============================================
 * 发包逻辑 — 丢包/延迟/乱序
 * ============================================ */

static void maybe_lose_and_delay(uv_udp_t *to_sock, struct sockaddr_in *dst,
                                  const uint8_t *data, size_t len,
                                  reorder_buf *rbuf) {
    /* 1. 丢包 */
    double r = (double)rand() / (double)RAND_MAX;
    if (r < g_loss_rate) {
        fprintf(stderr, "[proxy] DROP %zu bytes → %s:%d\n",
                len, inet_ntoa(dst->sin_addr), ntohs(dst->sin_port));
        return;
    }

    /* 2. 随机延迟 */
    int dly = g_delay_min +
              rand() % (g_delay_max - g_delay_min + 1);
    uint64_t fire_at = uv_now(g_loop) + (uint64_t)dly;

    /* 3. 乱序缓冲 */
    if (rbuf) {
        reorder_push(rbuf, data, len, dst, fire_at);
        if (rbuf->cnt >= g_reorder_n) {
            reorder_flush(rbuf);
        }
    } else {
        delay_enqueue(data, len, dst, fire_at);
    }
}

/* ============================================
 * UV recv 回调
 * ============================================ */

static void alloc_buffer(uv_handle_t *h, size_t suggested, uv_buf_t *buf) {
    (void)h;
    buf->base = (char*)malloc(suggested);
    buf->len  = (unsigned int)suggested;
}

static void on_face_recv(uv_udp_t *handle, ssize_t nread,
                          const uv_buf_t *buf,
                          const struct sockaddr *addr, unsigned flags) {
    (void)handle; (void)flags;
    if (nread <= 0) { free(buf->base); return; }

    /* 记录 Chrome 地址，用于回传 */
    memcpy(&g_chrome_addr, addr, sizeof(g_chrome_addr));
    g_chrome_bound = 1;

    struct sockaddr_in srv;
    memset(&srv, 0, sizeof(srv));
    srv.sin_family = AF_INET;
    srv.sin_port   = htons((uint16_t)g_server_port);
    inet_pton(AF_INET, g_server_ip, &srv.sin_addr);

    maybe_lose_and_delay(&g_back, &srv, (const uint8_t*)buf->base,
                          (size_t)nread, g_dir[0].rbuf);
    free(buf->base);
}

static void on_back_recv(uv_udp_t *handle, ssize_t nread,
                          const uv_buf_t *buf,
                          const struct sockaddr *addr, unsigned flags) {
    (void)handle; (void)addr; (void)flags;
    if (nread <= 0) { free(buf->base); return; }

    if (!g_chrome_bound) { free(buf->base); return; }

    maybe_lose_and_delay(&g_face, &g_chrome_addr,
                          (const uint8_t*)buf->base,
                          (size_t)nread, g_dir[1].rbuf);
    free(buf->base);
}

/* ============================================
 * 启动
 * ============================================ */

int main(int argc, char *argv[]) {
    int i;
    srand((unsigned int)time(NULL));

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--loss") == 0 && i + 1 < argc)
            g_loss_rate = atof(argv[++i]);
        else if (strcmp(argv[i], "--delay-min") == 0 && i + 1 < argc)
            g_delay_min = atoi(argv[++i]);
        else if (strcmp(argv[i], "--delay-max") == 0 && i + 1 < argc)
            g_delay_max = atoi(argv[++i]);
        else if (strcmp(argv[i], "--reorder") == 0 && i + 1 < argc)
            g_reorder_n = atoi(argv[++i]);
        else if (strcmp(argv[i], "--face-port") == 0 && i + 1 < argc)
            g_port_face = atoi(argv[++i]);
        else if (strcmp(argv[i], "--back-port") == 0 && i + 1 < argc)
            g_port_back = atoi(argv[++i]);
        else if (strcmp(argv[i], "--server") == 0 && i + 1 < argc)
            g_server_ip = argv[++i];
        else if (strcmp(argv[i], "--server-port") == 0 && i + 1 < argc)
            g_server_port = atoi(argv[++i]);
        else if (strcmp(argv[i], "--help") == 0) {
            printf("udp_jitter_proxy: inject loss/delay/reorder\n");
            printf("  --loss X        loss rate (0-1, default 0.10)\n");
            printf("  --delay-min N   min delay ms (default 5)\n");
            printf("  --delay-max N   max delay ms (default 50)\n");
            printf("  --reorder N     reorder buffer size (default 3, 0=off)\n");
            printf("  --face-port N   client-facing port (default 3443)\n");
            printf("  --back-port N   server-facing port (default 3444)\n");
            printf("  --server IP     server IP (default 127.0.0.1)\n");
            printf("  --server-port N server port (default 4433)\n");
            return 0;
        }
    }

    g_loop = uv_default_loop();

    /* ── bind face socket :3443 ── */
    {
        struct sockaddr_in addr;
        uv_ip4_addr("0.0.0.0", g_port_face, &addr);
        uv_udp_init(g_loop, &g_face);
        uv_udp_bind(&g_face, (const struct sockaddr*)&addr, 0);
        uv_udp_recv_start(&g_face, alloc_buffer, on_face_recv);
    }

    /* ── bind back socket :3444 ── */
    {
        struct sockaddr_in addr;
        uv_ip4_addr("0.0.0.0", g_port_back, &addr);
        uv_udp_init(g_loop, &g_back);
        uv_udp_bind(&g_back, (const struct sockaddr*)&addr, 0);
        uv_udp_recv_start(&g_back, alloc_buffer, on_back_recv);
    }

    /* ── 乱序缓冲 ── */
    g_dir[0].rbuf = (g_reorder_n > 0) ? reorder_create(g_reorder_n) : NULL;
    g_dir[1].rbuf = (g_reorder_n > 0) ? reorder_create(g_reorder_n) : NULL;

    /* ── 1ms drain 定时器 ── */
    uv_timer_init(g_loop, &g_drain_timer);
    uv_timer_start(&g_drain_timer, delay_drain, 0, 1);

    printf("[proxy] face :%d  <--->  back :%d  →  %s:%d\n",
           g_port_face, g_port_back, g_server_ip, g_server_port);
    printf("[proxy] loss=%.0f%%  delay=%d-%dms  reorder=%d\n",
           g_loss_rate * 100.0, g_delay_min, g_delay_max, g_reorder_n);

    uv_run(g_loop, UV_RUN_DEFAULT);

    /* cleanup */
    uv_timer_stop(&g_drain_timer);
    uv_close((uv_handle_t*)&g_drain_timer, NULL);
    uv_close((uv_handle_t*)&g_face, NULL);
    uv_close((uv_handle_t*)&g_back, NULL);
    reorder_free(g_dir[0].rbuf);
    reorder_free(g_dir[1].rbuf);

    while (g_delay_head) {
        delay_entry *e = g_delay_head;
        g_delay_head = e->next;
        free(e->data);
        free(e);
    }

    return 0;
}
