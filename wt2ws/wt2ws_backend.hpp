#ifndef WT2WS_BACKEND_HPP
#define WT2WS_BACKEND_HPP

#include "wt2ws_config.hpp"

#include "websocket_client.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace cpp_streamer {
namespace wt2ws {

/* Thin adapter around the repository's WebSocketClient.
 *
 * The proxy is the WebSocket *client*: it dials the backend (`ws://host:port/
 * path`, plain TCP inside the DC) and turns WS frames into callbacks.  Keeping
 * this behind one class means the relay never sees WebSocketClient details and
 * the WS implementation can be swapped later.
 */
class Wt2wsBackend : public WebSocketConnectionCallBackI {
public:
    struct Sink {
        std::function<void()> on_connected;
        std::function<void(const std::string &)> on_text;
        std::function<void(const uint8_t *, size_t)> on_binary;
        std::function<void(int, const std::string &)> on_closed;
    };

    Wt2wsBackend(uv_loop_t *loop, const Wt2wsConfig &cfg, Sink sink);
    ~Wt2wsBackend();

    Wt2wsBackend(const Wt2wsBackend &) = delete;
    Wt2wsBackend &operator=(const Wt2wsBackend &) = delete;

    void Connect();
    void SendText(const std::string &text);
    void SendBinary(const uint8_t *data, size_t len);
    void Close();
    bool connected() const { return connected_; }

    /* WebSocketConnectionCallBackI */
    void OnConnection() override;
    void OnReadText(int code, const std::string &text) override;
    void OnReadData(int code, const uint8_t *data, size_t len) override;
    void OnClose(int code, const std::string &desc) override;

private:
    std::unique_ptr<WebSocketClient> client_;
    Sink sink_;
    bool connected_ = false;
};

} /* namespace wt2ws */
} /* namespace cpp_streamer */

#endif /* WT2WS_BACKEND_HPP */
