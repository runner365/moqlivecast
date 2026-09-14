#include "co_http_server.hpp"
#include "net/tcp/co_tcp/co_tcp_server/co_tcp_accept_conn.hpp"
#include "utils/co_pub.hpp"

namespace cpp_streamer
{
CoHttpServer::CoHttpServer(uv_loop_t* loop, const std::string& host, uint16_t port, Logger* logger)
    : TimerInterface(200), logger_(logger)
{
    tcp_server_ptr_.reset(new CoTcpServer(loop, host, port, logger));
}

CoHttpServer::~CoHttpServer()
{
}

CoVoidTask CoHttpServer::Run()
{
    StartTimer();
    while(true) {
        std::shared_ptr<TcpCoAcceptConn> conn = co_await tcp_server_ptr_->CoAccept();
        if (conn->uv_tcp_handle_ == nullptr || conn->status_ != TCP_CONNECT_SUCCESS) {
            LogErrorf(logger_, "Failed to accept connection, status: %d", conn->status_);
            break;
        }
        
        std::string remote_endpoint = conn->GetRemoteEndpoint();
        std::shared_ptr<CoHttpSession> session = std::make_shared<CoHttpSession>(conn, this, logger_);

        http_sessions_[remote_endpoint] = session;

        session->HandleRequest();
    }
}

bool CoHttpServer::OnTimer() {
    for (auto it = http_sessions_.begin(); it != http_sessions_.end();) {
        if (!it->second->IsAlive()) {
            LogInfof(logger_, "HTTP session for endpoint %s is no longer alive, closing it.", it->first.c_str());
            it = http_sessions_.erase(it);
        } else {
            ++it;
        }
    }
    return timer_running_;
}

void CoHttpServer::AddGetHandle(const std::string& uri, CO_HTTP_HANDLE_PTR handle_func) {
    std::string get_uri(uri);
    std::string uri_key = GetUri(get_uri);
    get_handle_map_[uri_key] = handle_func;
}

void CoHttpServer::AddPostHandle(const std::string& uri, CO_HTTP_HANDLE_PTR handle_func) {
    std::string post_uri(uri);
    std::string uri_key = GetUri(post_uri);
    post_handle_map_[uri_key] = handle_func;
}

void CoHttpServer::OnClose(const std::string& endpoint) {
    auto it = http_sessions_.find(endpoint);
    if (it != http_sessions_.end()) {
        http_sessions_.erase(it);
        LogInfof(logger_, "Closed HTTP session for endpoint: %s", endpoint.c_str());
    } else {
        LogWarnf(logger_, "Attempted to close non-existent HTTP session for endpoint: %s", endpoint.c_str());
    }
}

CO_HTTP_HANDLE_PTR CoHttpServer::GetHandle(std::shared_ptr<CoHttpRequest> request) {
    std::string request_uri(request->uri_);
    std::string uri = GetUri(request_uri);
    
    if (request->method_ == "GET") {
        auto it = get_handle_map_.find(uri);
        if (it != get_handle_map_.end()) {
            return it->second;
        }
    } else if (request->method_ == "POST") {
        auto it = post_handle_map_.find(uri);
        if (it != post_handle_map_.end()) {
            return it->second;
        }
    }
    auto it = get_handle_map_.find("/");
    if (it != get_handle_map_.end()) {
        return it->second;
    }
    LogWarnf(logger_, "No handler found for HTTP request method: %s, URI: %s", request->method_.c_str(), uri.c_str());
    return nullptr;
}

}