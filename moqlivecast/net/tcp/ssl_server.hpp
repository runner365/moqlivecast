#ifndef SSL_SERVER_HPP
#define SSL_SERVER_HPP
#ifdef _WIN64
#define WIN32_LEAN_AND_MEAN  // ���� Windows �ɰ�����ͷ�ļ������� winsock.h��
#endif
#include "logger.hpp"
#include "ssl_pub.hpp"

#include <memory>
#include <string>
#include <stdint.h>
#include <iostream>
#include <stdio.h>
#include <queue>
#include <sstream>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <assert.h>

namespace cpp_streamer
{

class SslServer
{
public:
    SslServer(const std::string& key_file,
            const std::string& cert_file,
            SslCallbackI* cb,
            Logger* logger = nullptr):key_file_(key_file)
                          , cert_file_(cert_file)
                          , cb_(cb)
                          , logger_(logger)
    {
        plaintext_data_ = new uint8_t[plaintext_data_len_];
        LogInfof(logger_, "SslServer construct ...");
    }
    ~SslServer() {
        /* 先通知栈上正在执行的 HandleSslDataRecv：它可能在
         * cb_->PlaintextDataRecv() 返回后继续访问本对象。
         * 该回调会同步触发 CloseSession → 析构，属于重入销毁。 */
        if (alive_) *alive_ = false;

        if (ssl_) {
            SSL_free(ssl_);
            ssl_ = NULL;
        }

        if (ssl_ctx_) {
            SSL_CTX_free(ssl_ctx_);
            ssl_ctx_ = NULL;
        }
        if (plaintext_data_) {
            delete[] plaintext_data_;
            plaintext_data_ = nullptr;
        }
    }

public:
    TLS_SERVER_STATE GetState() {
        return tls_state_;
    }

    int Handshake(char* buf, size_t nn) {
        int ret = 0;

        if (tls_state_ >= TLS_SERVER_KEY_EXCHANGE_DONE) {
            return 0;
        }

        ret = SslInit();
        if (ret != 0) {
            return ret;
        }
        switch (tls_state_)
        {
            case TLS_SSL_SERVER_INIT_DONE:
            {
                ret = HandleTlsHello(buf, nn);
                if (ret != 0) {
                    return ret;
                }
                LogInfof(logger_, "SslServer received ClientHello, send ServerHello");
                tls_state_ = TLS_SSL_SERVER_HELLO_DONE;
                break;
            }
            case TLS_SSL_SERVER_HELLO_DONE:
            {
                ret = HandleKeyExchange(buf, nn);
                if (ret != 0) {
                    return ret;
                }
                break;
            }
            default:
                break;
        }
        return 0;
    }

    int HandleSslDataRecv(uint8_t* data, size_t len) {
        /* 局部拷贝存活标志：回调里若析构了本对象，*alive 会被置 false，
         * 而这个 shared_ptr 副本仍保证标志对象在栈帧内有效。
         * 之后一律用它判断，不再解引用 this 的任何成员。 */
        std::shared_ptr<bool> alive = alive_;

        if (!ssl_ || !bio_in_ || !bio_out_) {     // ← 检查 BIO 是否就绪
            LogErrorf(logger_, "HandleSslDataRecv: ssl/bio not ready, state=%d", tls_state_);
            return -1;
        }
        if (tls_state_ < TLS_SERVER_KEY_EXCHANGE_DONE) {
            return 0;
        }

        /* KeyExchange 已把本包写入 BIO。TLS1.3 常把 Finished 和 HTTP GET
         * 打在同一段，这里不能 return，必须接着 SSL_read。 */
        if (tls_state_ == TLS_SERVER_KEY_EXCHANGE_DONE) {
            LogInfof(logger_, "SslServer handshake done, drain app data, pkt=%zu", len);
            tls_state_ = TLS_SERVER_DATA_RECV_STATE;
        } else {
            int wr = BIO_write(bio_in_, data, (int)len);
            if (wr <= 0) {
                LogErrorf(logger_, "BIO_write error:%d", wr);
                return -1;
            }
        }

        while (true) {
            int r0 = SSL_read(ssl_, plaintext_data_, (int)plaintext_data_len_);
            int r1 = SSL_get_error(ssl_, r0);
            size_t r2 = BIO_ctrl_pending(bio_in_);
            if (r0 > 0) {
                LogInfof(logger_, "ssl plaintext %dB pending=%zu", r0, r2);
                cb_->PlaintextDataRecv((char*)plaintext_data_, r0);

                /* ★ 回调可能已同步析构本对象：HTTP 层判定非法请求
                 * （如 HTTP/2 的 "PRI * HTTP/2.0" 前言）时会立刻
                 * CloseSession → 释放 TcpSession/SslServer，SSL_free
                 * 连带释放 bio_in_/bio_out_。此时继续读成员就是 UAF，
                 * 实测崩溃在 BIO_ctrl_pending（BIO_ctrl）。
                 * 用局部 alive 判断，它是 shared_ptr 副本，安全。 */
                if (!*alive) {
                    LogInfof(logger_, "HandleSslDataRecv: destroyed by callback, abort");
                    return -1;
                }
            } else if (r1 == SSL_ERROR_WANT_READ || r1 == SSL_ERROR_WANT_WRITE) {
                break;
            } else {
                LogErrorf(logger_, "SSL_read error, r0:%d, r1:%d, pending=%zu, finished:%d",
                        r0, r1, r2, SSL_is_init_finished(ssl_));
                break;
            }
            if (r2 <= 0) {
                break;
            }
        }
        return 0;
    }

    int SslWrite(uint8_t* plain_text_data, size_t len) {
        int writen_len = 0;
        /* 与 HandleSslDataRecv 同样的重入销毁防护：回调之后不得再碰成员。
         * 发送方向目前不会同步析构（PlaintextDataSend 只做 uv_write），
         * 但保持一致的防御，改动成本为零。 */
        std::shared_ptr<bool> alive = alive_;

        for (char* p = (char*)plain_text_data; p < (char*)plain_text_data + len;) {
            int left = (int)len - (int)(p - (char*)plain_text_data);
            int r0 = SSL_write(ssl_, (const void*)p, left);
            int r1 = SSL_get_error(ssl_, r0);
            if (r0 <= 0) {
                LogErrorf(logger_, "ssl write data=%p, size=%d, r0=%d, r1=%d",
                        p, left, r0, r1);
                return -1;
            }

            // Move p to the next writing position.
            p += r0;
            writen_len += (ssize_t)r0;

            uint8_t* data = NULL;
            int size = BIO_get_mem_data(bio_out_, &data);
            cb_->PlaintextDataSend((char*)data, size);

            if (!*alive) return -1;   /* 回调销毁了本对象，安全退出 */

            if ((r0 = BIO_reset(bio_out_)) != 1) {
                LogErrorf(logger_, "BIO_reset r0=%d", r0);
                return -1;
            }
        }

        return writen_len;
    }

private:
    void LogSSLErrors(const char* ctx) {
        unsigned long e = 0;
        char buf[256] = {0};
        while ((e = ERR_get_error()) != 0) {
            ERR_error_string_n(e, buf, sizeof(buf));
            LogErrorf(logger_, "%s: OpenSSL error: %s (ERR %lu)", ctx, buf, e);
        }
    }

    int SslInit() {
        if (tls_state_ >= TLS_SSL_SERVER_INIT_DONE) {
            return 0;
        }

#if (OPENSSL_VERSION_NUMBER >= 0x10100000L)
        OPENSSL_init_ssl(OPENSSL_INIT_LOAD_SSL_STRINGS | OPENSSL_INIT_LOAD_CRYPTO_STRINGS, NULL);
#else
        SSL_load_error_strings();
        OpenSSL_add_all_algorithms();
#endif
    #if (OPENSSL_VERSION_NUMBER < 0x10100000L) // v1.1.0
        ssl_ctx_ = SSL_CTX_new(TLSv1_2_method());
    #else
        ssl_ctx_ = SSL_CTX_new(TLS_server_method());
        SSL_CTX_set_min_proto_version(ssl_ctx_, TLS1_2_VERSION);
    #endif
        SSL_CTX_set_verify(ssl_ctx_, SSL_VERIFY_NONE, NULL);
        assert(SSL_CTX_set_cipher_list(ssl_ctx_, "ALL") == 1);

        if ((bio_in_ = BIO_new(BIO_s_mem())) == NULL) {
            LogErrorf(logger_, "BIO_new in error");
            return -1;
        }
    
        if ((bio_out_ = BIO_new(BIO_s_mem())) == NULL) {
            LogErrorf(logger_, "BIO_new in error");
            BIO_free(bio_in_);
            return -1;
        }
    
        // Load certificate chain and private key into the SSL_CTX so
        // intermediate certificates are sent to clients.
        int r0;
        if ((r0 = SSL_CTX_use_certificate_chain_file(ssl_ctx_, cert_file_.c_str())) != 1) {
            LogErrorf(logger_, "SSL_CTX_use_certificate_chain_file error, cert file:%s, return %d", cert_file_.c_str(), r0);
            LogSSLErrors("SSL_CTX_use_certificate_chain_file");
            return -1;
        }

        if ((r0 = SSL_CTX_use_PrivateKey_file(ssl_ctx_, key_file_.c_str(), SSL_FILETYPE_PEM)) != 1) {
            LogErrorf(logger_, "SSL_CTX_use_PrivateKey_file error, key file:%s, return %d", key_file_.c_str(), r0);
            LogSSLErrors("SSL_CTX_use_PrivateKey_file");
            return -1;
        }

        if ((r0 = SSL_CTX_check_private_key(ssl_ctx_)) != 1) {
            LogErrorf(logger_, "SSL_CTX_check_private_key error, return %d", r0);
            LogSSLErrors("SSL_CTX_check_private_key");
            return -1;
        }

        // Now create the SSL object after ctx has certs/keys loaded.
        if ((ssl_ = SSL_new(ssl_ctx_)) == NULL) {
            LogErrorf(logger_, "ssl new error");
            return -1;
        }

        SSL_set_bio(ssl_, bio_in_, bio_out_);

        // SSL setup active, as server role.
        SSL_set_accept_state(ssl_);
        SSL_set_mode(ssl_, SSL_MODE_ENABLE_PARTIAL_WRITE);
        tls_state_ = TLS_SSL_SERVER_INIT_DONE;

        LogInfof(logger_, "ssl init done ....");
        
        return 0;
    }


    int HandleTlsHello(char* buf, size_t nn) {
        int r0 = 0;
        int r1 = 0;
        size_t size = 0;
        uint8_t* data = nullptr;

        if ((r0 = BIO_write(bio_in_, buf, (int)nn)) <= 0) {
            LogErrorf(logger_, "client hello BIO_write error:%d", r0);
            return -1;
        }
    
        r0 = SSL_do_handshake(ssl_);
        r1 = SSL_get_error(ssl_, r0);
        if (r0 <= 0) {
            if (r1 == SSL_ERROR_WANT_READ || r1 == SSL_ERROR_WANT_WRITE) {
                // non-fatal, handshake needs more data or needs to write
            } else {
                LogErrorf(logger_, "client hello BIO_write error r0:%d, r1:%d", r0, r1);
                LogSSLErrors("HandleTlsHello:SSL_do_handshake");
                return -1;
            }
        }
    
        if ((size = BIO_get_mem_data(bio_out_, &data)) > 0) {
            if ((r0 = BIO_reset(bio_in_)) != 1) {
                LogErrorf(logger_, "BIO_reset error:%d", r0);
                return -1;
            }
        }

        size = BIO_get_mem_data(bio_out_, &data);
        if (!data || size <= 0) {
            LogErrorf(logger_, "BIO_get_mem_data error");
            return -1;
        }

        cb_->PlaintextDataSend((char*)data, size);

        if ((r0 = BIO_reset(bio_out_)) != 1) {
            LogErrorf(logger_, "BIO_reset error:%d", r0);
            return -1;
        }

        return 0;
    }

    int HandleKeyExchange(char* buf, size_t nn) {
        int r0 = 0;
        int r1 = 0;
        size_t size = 0;
        char* data  = nullptr;

        if ((r0 = BIO_write(bio_in_, buf, (int)nn)) <= 0) {
            LogErrorf(logger_, "BIO_write error:%d", r0);
            return -1;
        }

        r0 = SSL_do_handshake(ssl_);
        r1 = SSL_get_error(ssl_, r0);
        if (r0 == 1 && r1 == SSL_ERROR_NONE) {
            size = BIO_get_mem_data(bio_out_, &data);
            if (!data || size <= 0) {
                LogErrorf(logger_, "BIO_get_mem_data error");
                return -1;
            }
        } else {
            if (r0 <= 0) {
                if (r1 == SSL_ERROR_WANT_READ || r1 == SSL_ERROR_WANT_WRITE) {
                    /* non-fatal, continue */
                } else {
                    LogErrorf(logger_, "handle key exchange SSL_do_handshake error, r0:%d, r1:%d", r0, r1);
                    LogSSLErrors("HandleKeyExchange:SSL_do_handshake");
                    return -1;
                }
            }

            if ((size = BIO_get_mem_data(bio_out_, &data)) > 0) {
                if ((r0 = BIO_reset(bio_in_)) != 1) {
                    LogErrorf(logger_, "BIO_reset error");
                    return -1;
                }
            }
        }

        // Send New Session Ticket, Change Cipher Spec, Encrypted Handshake Message
        cb_->PlaintextDataSend((char*)data, size);

        if ((r0 = BIO_reset(bio_out_)) != 1) {
            LogErrorf(logger_, "BIO_reset error");
            return -1;
        }
        LogInfof(logger_, "SslServer completed KeyExchange.");
        tls_state_ = TLS_SERVER_KEY_EXCHANGE_DONE;
        return 0;
    }

private:
    std::string key_file_;
    std::string cert_file_;
    SslCallbackI* cb_ = nullptr;
    Logger* logger_ = nullptr;

private:
    SSL_CTX* ssl_ctx_ = nullptr;
    SSL* ssl_         = nullptr;
    BIO* bio_in_      = nullptr;
    BIO* bio_out_     = nullptr;
    uint8_t* plaintext_data_ = nullptr;
    size_t plaintext_data_len_ = SSL_DEF_RECV_BUFFER_SIZE;

private:
    TLS_SERVER_STATE tls_state_ = TLS_SSL_SERVER_ZERO;

    /* 存活标志。用 shared_ptr 是为了让 HandleSslDataRecv 能取一份
     * 局部副本：回调同步析构本对象时，标志对象本身不能随之失效，
     * 否则判断存活这一步又是 UAF。析构时置 false。 */
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
};

}
#endif //SSL_SERVER_HPP
