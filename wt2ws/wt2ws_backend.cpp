#include "wt2ws_backend.hpp"

#include "logger.h"

#include <map>
#include <utility>

namespace cpp_streamer {
namespace wt2ws {

Wt2wsBackend::Wt2wsBackend(uv_loop_t *loop, const Wt2wsConfig &cfg, Sink sink)
    : sink_(std::move(sink)) {
    /* The C++ Logger* is optional in this stack; the proxy logs through the C
     * logger so the two sides share one log file. */
    client_.reset(new WebSocketClient(loop, cfg.backend_host, cfg.backend_port,
                                      cfg.backend_path, cfg.backend_ssl,
                                      nullptr, this));
}

Wt2wsBackend::~Wt2wsBackend() {
    Close();
}

void Wt2wsBackend::Connect() {
    std::map<std::string, std::string> headers;
    headers["X-Wt2ws"] = "1";
    try {
        client_->AsyncConnect(headers);
    } catch (const std::exception &e) {
        /* DNS/uv_tcp_connect errors throw from TcpClient::Connect.  Treat it
         * as a failed handshake; the session's connect deadline also covers
         * the silent-failure case. */
        LOG_ERROR("[wt2ws] backend connect threw: %s", e.what());
        connected_ = false;
        if (sink_.on_closed) sink_.on_closed(-1, e.what());
    }
}

void Wt2wsBackend::SendText(const std::string &text) {
    if (!connected_) return;
    try {
        client_->AsyncWriteText(text);
    } catch (const std::exception &e) {
        LOG_ERROR("[wt2ws] backend send failed: %s", e.what());
        Close();
    }
}

void Wt2wsBackend::SendBinary(const uint8_t *data, size_t len) {
    if (!connected_ || (!data && len)) return;
    try {
        client_->AsyncWriteData(data, len);
    } catch (const std::exception &e) {
        LOG_ERROR("[wt2ws] backend send failed: %s", e.what());
        Close();
    }
}

void Wt2wsBackend::Close() {
    connected_ = false;
    if (client_) {
        client_->Close();
    }
}

void Wt2wsBackend::OnConnection() {
    connected_ = true;
    LOG_INFO("[wt2ws] backend WebSocket handshake ok");
    if (sink_.on_connected) sink_.on_connected();
}

void Wt2wsBackend::OnReadText(int code, const std::string &text) {
    (void)code;
    if (sink_.on_text) sink_.on_text(text);
}

void Wt2wsBackend::OnReadData(int code, const uint8_t *data, size_t len) {
    (void)code;
    if (sink_.on_binary) sink_.on_binary(data, len);
}

void Wt2wsBackend::OnClose(int code, const std::string &desc) {
    if (!connected_ && code == 0) {
        /* Close() was called locally: the object is being torn down. */
    }
    connected_ = false;
    LOG_INFO("[wt2ws] backend closed code=%d reason=%s", code, desc.c_str());
    if (sink_.on_closed) sink_.on_closed(code, desc);
}

} /* namespace wt2ws */
} /* namespace cpp_streamer */
