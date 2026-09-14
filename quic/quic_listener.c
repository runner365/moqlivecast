#include "quic_listener.h"
#include "quic_connection.h"
#include "quic_timer.h"
#include "quic_packet.h"
#include "quic_crypto.h"
#include "tls_common.h"
#include "logger.h"
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <openssl/rand.h>
#include <openssl/hmac.h>

#define MAX_CONNS          128
#define MAX_RETRY_TOKENS    32
#define RETRY_TOKEN_EXPIRY 30000  /* 30s expiry (ms) */

typedef struct {
    QuicConnectionId dcid;
    uint8_t          token[16];
    uint64_t         expire_ms;     /* uv_now() 到期时间戳 */
} RetryTokenEntry;

struct QuicListener {
    uv_loop_t *loop_;
    SSL_CTX   *ssl_ctx_;
    uv_udp_t   udp_;
    uint8_t    rbuf_[65536];
    int        bound_;
    int        retry_enabled_;   /* 是否启用 Retry 地址验证 */

    /* Stateless Reset (RFC 9000 §10.3): 32 字节静态密钥 */
    uint8_t    reset_key_[32];

    /* Retry token 表 — 简单 LRU */
    RetryTokenEntry retry_tokens_[MAX_RETRY_TOKENS];
    int             retry_token_cnt_;

    QuicConnection **conns_;
    size_t          conn_cnt_;
    size_t          conn_cap_;

    QuicListenerOnConnection on_connection_;
    void *app_data_;
};

/* ============================================
 * 前向声明
 * ============================================ */
static void on_udp_alloc(uv_handle_t *h, size_t suggested, uv_buf_t *buf);
static void on_udp_recv(uv_udp_t *h, ssize_t nread, const uv_buf_t *buf,
                        const struct sockaddr *addr, unsigned flags);
static void on_udp_close(uv_handle_t *h);
static QuicConnection* find_conn(QuicListener *l, const QuicConnectionId *dcid);
static void add_conn(QuicListener *l, QuicConnection *conn);
static void remove_conn(QuicListener *l, QuicConnection *conn);

/* ============================================
 * 连接关闭转发
 * ============================================ */
static void conn_on_close(QuicConnection *conn, uint64_t error_code,
                          const char *reason) {
    LOG_DEBUG("[quic-listener] connection closed: err=%llu", (unsigned long long)error_code);
    QuicListener *l = (QuicListener*)QuicConnectionGetListener(conn);
    if (l) remove_conn(l, conn);
}

/* ============================================
 * 构造 / 析构
 * ============================================ */

QuicListener* QuicListenerCreate(uv_loop_t *loop,
                                 const char *cert_file, const char *key_file) {
    quic_crypto_init();

    QuicListener *l = (QuicListener*)calloc(1, sizeof(QuicListener));
    if (!l) return NULL;

    l->loop_ = loop;
    quic_timer_wheel_init(loop);

    l->ssl_ctx_ = tls_ctx_server_new(cert_file, key_file);
    if (!l->ssl_ctx_) {
        LOG_ERROR("[quic-listener] create SSL_CTX failed");
        free(l);
        return NULL;
    }
    tls_ctx_set_alpn(l->ssl_ctx_);

    l->conn_cap_ = 16;
    l->conns_ = (QuicConnection**)calloc(l->conn_cap_, sizeof(QuicConnection*));
    if (!l->conns_) {
        tls_ctx_free(l->ssl_ctx_);
        free(l);
        return NULL;
    }

    uv_udp_init(loop, &l->udp_);
    l->udp_.data = l;

    /* 生成 Stateless Reset 静态密钥（一次性随机） */
    RAND_bytes(l->reset_key_, sizeof(l->reset_key_));

    return l;
}

void QuicListenerDestruct(QuicListener *l) {
    if (!l) return;

    /* 关闭所有连接 */
    for (size_t i = 0; i < l->conn_cnt_; i++) {
        if (l->conns_[i]) {
            QuicConnectionDestruct(l->conns_[i]);
        }
    }

    if (l->bound_) {
        uv_close((uv_handle_t*)&l->udp_, on_udp_close);
    } else {
        tls_ctx_free(l->ssl_ctx_);
        free(l->conns_);
        free(l);
    }
}

void QuicListenerSetOnConnection(QuicListener *l,
                                 QuicListenerOnConnection cb) {
    l->on_connection_ = cb;
}

void QuicListenerSetAppData(QuicListener *l, void *data) { l->app_data_ = data; }
void* QuicListenerGetAppData(QuicListener *l) { return l->app_data_; }

void QuicListenerSetRetryEnabled(QuicListener *l, int enabled) {
    l->retry_enabled_ = enabled;
}

int QuicListenerListen(QuicListener *l, const char *ip, int port) {
    struct sockaddr_in addr;
    int ret = uv_ip4_addr(ip, port, &addr);
    if (ret < 0) {
        LOG_ERROR("[quic-listener] ip4_addr failed: %s", uv_strerror(ret));
        return ret;
    }

    ret = uv_udp_bind(&l->udp_, (const struct sockaddr*)&addr, 0);
    if (ret < 0) {
        LOG_ERROR("[quic-listener] bind failed: %s", uv_strerror(ret));
        return ret;
    }

    ret = uv_udp_recv_start(&l->udp_, on_udp_alloc, on_udp_recv);
    if (ret < 0) {
        LOG_ERROR("[quic-listener] recv_start failed: %s", uv_strerror(ret));
        return ret;
    }

    l->bound_ = 1;
    LOG_DEBUG("[quic-listener] listening on %s:%d", ip, port);
    return 0;
}

/* ============================================
 * UDP 回调
 * ============================================ */

static void on_udp_alloc(uv_handle_t *h, size_t suggested, uv_buf_t *buf) {
    QuicListener *l = (QuicListener*)h->data;
    buf->base = (char*)l->rbuf_;
    buf->len  = sizeof(l->rbuf_);
}

static void on_udp_recv(uv_udp_t *h, ssize_t nread, const uv_buf_t *buf,
                        const struct sockaddr *addr, unsigned flags) {
    QuicListener *l = (QuicListener*)h->data;
    if (nread <= 0) return;

    char ip[64];
    int port = 0;
    if (addr->sa_family == AF_INET) {
        struct sockaddr_in *in = (struct sockaddr_in*)addr;
        uv_ip4_name(in, ip, sizeof(ip));
        port = ntohs(in->sin_port);
    }

    const uint8_t *data = (const uint8_t*)buf->base;
    size_t len = (size_t)nread;

    if (len < 1) return;

    int is_long = (data[0] & 0x80) != 0;

    if (is_long) {
        /* 长头包：提取 DCID 查找连接 */
        if (len < 6) return; /* 至少 1+4+1 */

        size_t pos = 1 + 4; /* 跳过首字节 + version */
        uint32_t version_n;
        memcpy(&version_n, data + 1, 4);
        uint32_t quic_version = ntohl(version_n);
        uint8_t dcid_len = data[pos++];
        LOG_DEBUG("[quic-listener] Initial: version=0x%08x dcid_len=%u",
                 quic_version, dcid_len);
        if (pos + dcid_len > len) return;

        const uint8_t *dcid_bytes = data + pos; /* 保存原始 DCID 指针（密钥派生需要原始长度） */
        QuicConnectionId dcid;
        memset(&dcid, 0, sizeof(dcid));
        if (dcid_len > 0 && dcid_len <= QUIC_CID_MAX_LEN) {
            memcpy(dcid.data, dcid_bytes, dcid_len);
            dcid.len = dcid_len;
        } else {
            dcid.len = 0;
        }
        pos += dcid_len;

        QuicConnection *conn = find_conn(l, &dcid);

        if (conn) {
            /* 已有连接 → 直接转发原始数据 */
            QuicConnectionFeedRaw(conn, data, len, ip, port);
            /* 延迟握手模式下，每次新数据到齐后尝试触发握手 */
            QuicConnectionCryptoFlushHandshake(conn);
            return;
        }

        /* 新连接 — 只处理 Initial 包 */
        int raw_type = (data[0] >> 4) & 0x03;
        if (raw_type != QUIC_PKT_INITIAL) {
            LOG_DEBUG("[quic-listener] drop non-Initial packet for unknown DCID");
            return;
        }

        /* 提取 SCID */
        if (pos >= len) return;
        uint8_t scid_len = data[pos++];
        if (pos + scid_len > len) return;
        QuicConnectionId scid;
        memset(&scid, 0, sizeof(scid));
        if (scid_len > 0 && scid_len <= QUIC_CID_MAX_LEN) {
            memcpy(scid.data, data + pos, scid_len);
            scid.len = scid_len;
        } else {
            scid.len = 0;
        }
        pos += scid_len;

        LOG_DEBUG("[quic-listener] new Initial from %s:%d (dcid=%02x%02x.. scid=%02x%02x..)",
               ip, port, dcid.data[0], dcid.data[1], scid.data[0], scid.data[1]);

        /* Retry 地址验证 (RFC 9000 §8.1.2) — 全新连接时先发 Retry */
        if (l->retry_enabled_) {
            uint64_t now = uv_now(l->loop_);

            /* 检查 token: 如果 Initial 包含有效 Retry token → 放行 */
            int token_valid = 0;
            {
                /* 重新解析 token — 在 parse_long 之前我们不知道 token 在哪。
                 * 用一个简单扫描判断 token 位置：找到 SCID 之后的 varint。 */
                /* 简化：listener 尚未调 parse_long，手动快扫 token */
                size_t tp = pos;
                /* skip Initial packet type-specific: token varint + token */
                uint64_t tok_len = 0;
                int tv = quic_varint_decode(data + tp, len - tp, &tok_len);
                if (tv > 0 && tok_len > 0 && tok_len < len - tp && tok_len >= 16) {
                    tp += tv;
                    const uint8_t *tok_data = data + tp + tok_len - 16;
                    /* 检查 token 的最后 16 字节是否匹配 */
                    for (int i = 0; i < l->retry_token_cnt_; i++) {
                        if (now > l->retry_tokens_[i].expire_ms) continue;
                        if (memcmp(tok_data, l->retry_tokens_[i].token, 16) == 0) {
                            token_valid = 1;
                            LOG_DEBUG("[quic-listener] Retry token validated");
                            break;
                        }
                    }
                }
            }

            /* 清理过期 token */
            for (int i = 0; i < l->retry_token_cnt_; ) {
                if (now > l->retry_tokens_[i].expire_ms)
                    l->retry_tokens_[i] = l->retry_tokens_[--l->retry_token_cnt_];
                else i++;
            }

            if (!token_valid) {
                /* 发送 Retry */
                QuicConnectionId retry_scid;
                memset(&retry_scid, 0, sizeof(retry_scid));
                RAND_bytes(retry_scid.data, QUIC_CID_LEN);
                retry_scid.len = QUIC_CID_LEN;

                uint8_t retry_token[16];
                RAND_bytes(retry_token, 16);

                if (l->retry_token_cnt_ < MAX_RETRY_TOKENS) {
                    l->retry_tokens_[l->retry_token_cnt_].expire_ms =
                        now + RETRY_TOKEN_EXPIRY;
                    memcpy(l->retry_tokens_[l->retry_token_cnt_].token,
                           retry_token, 16);
                    l->retry_token_cnt_++;
                }

                uint8_t retry_buf[2048];
                size_t retry_len;
                if (quic_packet_build_retry(retry_buf, &retry_len,
                                             &dcid, &retry_scid,
                                             retry_token, 16) == 0) {
                    struct sockaddr_in peer;
                    memset(&peer, 0, sizeof(peer));
                    peer.sin_family = AF_INET;
                    peer.sin_port   = htons((unsigned short)port);
                    uv_ip4_addr(ip, port, &peer);
                    uv_buf_t ub = uv_buf_init((char*)retry_buf,
                                              (unsigned int)retry_len);
                    uv_udp_try_send(&l->udp_, &ub, 1,
                                    (const struct sockaddr*)&peer);
                    LOG_DEBUG("[quic-listener] sent Retry to %s:%d",
                             ip, port);
                }
                return;
            }
        }

        /* 创建连接 */
        conn = QuicConnectionCreate(l->loop_);
        if (!conn) {
            LOG_ERROR("[quic-listener] create conn failed");
            return;
        }

        /* 设置关闭回调 + 反向引用 */
        QuicConnectionSetOnClose(conn, conn_on_close);
        QuicConnectionSetListener(conn, l);

        /* 派生 initial 密钥 — 使用原始 DCID 字节（长度可能 > QUIC_CID_LEN） */
        QuicCipherKeys client_ikeys, server_ikeys;
        if (quic_crypto_derive_initial_keys(&client_ikeys, &server_ikeys,
                                             dcid_bytes, dcid_len) < 0) {
            LOG_ERROR("[quic-listener] derive initial keys failed");
            QuicConnectionDestruct(conn);
            return;
        }

        /* 接受连接（延迟握手：do_ssl_accept=0，不在此调 SSL_accept） */
        int ret = QuicConnectionAccept(conn, l->ssl_ctx_, &l->udp_, ip, port,
                                        &scid, &dcid,
                                        &client_ikeys, &server_ikeys,
                                        NULL, 0, 0 /* do_ssl_accept=0 */);
        if (ret < 0) {
            LOG_ERROR("[quic-listener] accept failed");
            QuicConnectionDestruct(conn);
            return;
        }

        add_conn(l, conn);

        if (l->on_connection_) {
            l->on_connection_(l, conn, ip, port);
        }

        /* 首包统一走 FeedRaw — 解密 + record_rx_pn + ACK + CRYPTO gap 处理
         * + SSL_do_handshake 驱动 + coalesced 包推进，全在连接内完成。
         * 反放大预算也在 FeedRaw 内计入（bytes_recv_addr_val_ += len）。 */
        QuicConnectionFeedRaw(conn, data, len, ip, port);
        QuicConnectionCryptoFlushHandshake(conn);

    } else {
        /* 短头 — 遍历连接以匹配可变长度 DCID */
        LOG_DEBUG("[quic-listener] short header packet, len=%zu, checking %zu conns",
                  len, l->conn_cnt_);
        for (size_t i = 0; i < l->conn_cnt_; i++) {
            QuicConnection *c = l->conns_[i];
            if (!c) continue;
            const QuicConnectionId *scid = QuicConnectionGetSrcCid(c);
            LOG_DEBUG("[quic-listener]   conn[%zu]: scid_len=%u, pkt_cid=%02x%02x..",
                      i, scid->len,
                      len > 1 ? data[1] : 0, len > 2 ? data[2] : 0);
            if (len < 1 + (size_t)scid->len) continue;
            QuicConnectionId dcid;
            memcpy(dcid.data, data + 1, scid->len);
            dcid.len = scid->len;
            if (quic_cid_eq(&dcid, scid)) {
                LOG_DEBUG("[quic-listener] short header matched conn[%zu], feeding", i);
                QuicConnectionFeedRaw(c, data, len, ip, port);
                return;
            }
        }
        /* RFC 9000 §10.3: 无匹配连接 → 尝试发送 Stateless Reset */
        if (len >= 1 + 8 && len >= 21) {
            /* 使用常见 DCID 长度 (8) 计算 reset token */
            uint8_t dcid_try[8];
            memcpy(dcid_try, data + 1, 8);

            unsigned int md_len = 0;
            uint8_t md[32];
            HMAC(EVP_sha256(), l->reset_key_, 32,
                 dcid_try, 8, md, &md_len);

            /* 构造 reset 包: rand_header + rand_body + token(16B) */
            uint8_t reset[43];
            RAND_bytes(reset, sizeof(reset));
            reset[0] = (reset[0] & 0x1f) | 0x40;  /* short header look-alike */
            memcpy(reset + sizeof(reset) - 16, md, 16);  /* token 在尾部 */

            struct sockaddr_in peer;
            memset(&peer, 0, sizeof(peer));
            peer.sin_family = AF_INET;
            peer.sin_port   = htons((unsigned short)port);
            uv_ip4_addr(ip, port, &peer);

            uv_buf_t ub = uv_buf_init((char*)reset, sizeof(reset));
            uv_udp_try_send(&l->udp_, &ub, 1, (const struct sockaddr*)&peer);
            LOG_DEBUG("[quic-listener] sent stateless reset to %s:%d", ip, port);
        }
    }
}

static void on_udp_close(uv_handle_t *h) {
    QuicListener *l = (QuicListener*)h->data;
    tls_ctx_free(l->ssl_ctx_);
    free(l->conns_);
    free(l);
}

/* ============================================
 * 连接管理
 * ============================================ */

static QuicConnection* find_conn(QuicListener *l, const QuicConnectionId *dcid) {
    for (size_t i = 0; i < l->conn_cnt_; i++) {
        if (l->conns_[i]) {
            const QuicConnectionId *scid = QuicConnectionGetSrcCid(l->conns_[i]);
            if (quic_cid_eq(scid, dcid)) {
                return l->conns_[i];
            }
        }
    }
    return NULL;
}

static void add_conn(QuicListener *l, QuicConnection *conn) {
    if (l->conn_cnt_ >= l->conn_cap_) {
        size_t new_cap = l->conn_cap_ * 2;
        QuicConnection **new_arr = (QuicConnection**)realloc(
            l->conns_, new_cap * sizeof(QuicConnection*));
        if (!new_arr) return;
        l->conns_ = new_arr;
        l->conn_cap_ = new_cap;
    }
    l->conns_[l->conn_cnt_++] = conn;
}

static void remove_conn(QuicListener *l, QuicConnection *conn) {
    for (size_t i = 0; i < l->conn_cnt_; i++) {
        if (l->conns_[i] == conn) {
            l->conns_[i] = l->conns_[--l->conn_cnt_];
            l->conns_[l->conn_cnt_] = NULL;
            return;
        }
    }
}

void QuicListenerRemoveConn(QuicListener *l, QuicConnection *conn) {
    if (l && conn) remove_conn(l, conn);
}
