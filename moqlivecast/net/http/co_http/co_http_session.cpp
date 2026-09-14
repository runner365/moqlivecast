#include "co_http_session.hpp"
#include "net/tcp/co_tcp/co_tcp_server/co_tcp_session_recv.hpp"
#include "net/tcp/co_tcp/co_tcp_server/co_tcp_session_send.hpp"
#include "net/tcp/co_tcp/co_tcp_server/co_tcp_accept_conn.hpp"
#include "utils/stringex.hpp"
#include "utils/logger.hpp"
#include "utils/timeex.hpp"

namespace cpp_streamer
{
CoHttpSession::CoHttpSession(std::shared_ptr<TcpCoAcceptConn> accept_conn, CoHttpCallbackI* cb, Logger* logger):
    accept_conn_(accept_conn), cb_(cb), logger_(logger)
{
    header_data_ = std::make_shared<DataBuffer>();
    content_data_ = std::make_shared<DataBuffer>();
    alive_ms_ = now_millisec();
}

CoHttpSession::~CoHttpSession()
{
}

CoVoidTask CoHttpSession::HandleRequest() {
    const size_t buffer_size = 10 * 1024;
    uint8_t buffer[buffer_size] = {0};  
    
    while (true) {
        alive_ms_ = now_millisec();
        RecvResult recv_result = co_await accept_conn_->CoReceive(buffer, buffer_size);
        if (recv_result.status != RECV_OK) {
            LogErrorf(logger_, "Failed to receive HTTP request, status: %d", recv_result.status);
            co_return;
        }

        alive_ms_ = now_millisec();
        if (!header_is_ready_) {
            //get http header and put it into request_
            int ret = AnalyzeHeader(buffer, recv_result.recv_len);
            if (ret == 0) {
                continue; // Header not ready, wait for more data
            } else if (ret < 0) {
                LogErrorf(logger_, "Failed to analyze HTTP header");
                co_return;
            }
        } else {
            // Append content data
            content_data_->AppendData(reinterpret_cast<const char*>(buffer), recv_result.recv_len);
        }

        if (request_->method_ == "GET") {
            CO_HTTP_HANDLE_PTR handle_ptr = cb_->GetHandle(request_);
            if (!handle_ptr) {
                LogErrorf(logger_, "Failed to get HTTP handle for GET request, subpath:%s", request_->uri_.c_str());
                co_return;
            }
            response_ = std::make_shared<CoHttpResponse>(accept_conn_, this, logger_);
            try {
                alive_ms_ = now_millisec();
                handle_ptr(request_, response_);
            } catch(const std::exception& e) {
                LogErrorf(logger_, "Exception in HTTP GET handler: %s", e.what());
            }
            co_return;
        }

        if (request_->content_length_ > 0 && (int)content_data_->DataLen() >= request_->content_length_) {
            CO_HTTP_HANDLE_PTR handle_ptr = cb_->GetHandle(request_);
            if (!handle_ptr) {
                LogErrorf(logger_, "Failed to get HTTP handle for POST request, subpath:%s", request_->uri_.c_str());
                co_return;
            }
            if (!response_) {
                response_ = std::make_shared<CoHttpResponse>(accept_conn_, this, logger_);
            }
            request_->content_data_ = content_data_;
            try {
                alive_ms_ = now_millisec();
                handle_ptr(request_, response_);
            } catch(const std::exception& e) {
                LogErrorf(logger_, "Exception in HTTP POST handler: %s", e.what());
            }
            co_return;
        }
    }
}

int CoHttpSession::AnalyzeHeader(const uint8_t* data, size_t data_size) {
    if (header_is_ready_) {
        return 1;
    }
    header_data_->AppendData(reinterpret_cast<const char*>(data), data_size);

    std::string info(header_data_->Data(), header_data_->DataLen());
    size_t pos = info.find("\r\n\r\n");
    if (pos == info.npos) {
        header_is_ready_ = false;
        return 0;
    }
    //http header is ready
    content_start_ = (int)pos + 4;
    header_is_ready_ = true;

    request_ = std::make_shared<CoHttpRequest>();
    request_->header_data_->AppendData(header_data_->Data(), pos);

    char* start = header_data_->Data() + content_start_;
    int len     = (int)header_data_->DataLen() - content_start_;
    content_data_->AppendData(start, len);

    std::string header_str(header_data_->Data(), pos);
    std::vector<std::string> header_vec;

    StringSplit(header_str, "\r\n", header_vec);
    if (header_vec.empty()) {
        return -1;
    }

    const std::string& first_line = header_vec[0];
    std::vector<std::string> version_vec;
    StringSplit(first_line, " ", version_vec);
    if (version_vec.size() != 3) {
        LogErrorf(logger_, "version line error:%s", first_line.c_str());
        return -1;
    }

    request_->method_ = version_vec[0];

    if (GetUriAndParams(version_vec[1], request_->uri_, request_->params) < 0) {
        LogErrorf(logger_, "the uri is error:%s", version_vec[1].c_str());
        return -1;
    }

    const std::string& version_info = version_vec[2];
    pos = version_info.find("HTTP");
    if (pos == version_info.npos) {
        LogErrorf(logger_, "http version error:%s", version_info.c_str());
        return -1;
    }

    request_->version_ = version_info.substr(pos+5);
    for (size_t index = 1; index < header_vec.size(); index++) {
        const std::string& info_line = header_vec[index];
        pos = info_line.find(":");
        if (pos == info_line.npos) {
            LogErrorf(logger_, "header line error:%s", info_line.c_str());
            return -1;
        }
        std::string key = info_line.substr(0, pos);
        std::string value = info_line.substr(pos+2);
        if (key == "Content-Length") {
            request_->content_length_ = atoi(value.c_str());
        }
        request_->headers_.insert(std::make_pair(key, value));
    }

    return 1;// Header is ready
}

void CoHttpSession::OnResponseAlive() {
    alive_ms_ = now_millisec();
}

void CoHttpSession::OnResponseClose() {
    is_close_ = true;
    cb_->OnClose(accept_conn_->GetRemoteEndpoint());
}

bool CoHttpSession::IsAlive() const {
    return now_millisec() - alive_ms_ < 5000 && !is_close_;
}

} // namespace cpp_streamer