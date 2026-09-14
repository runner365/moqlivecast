#include "co_http_common.hpp"
#include "net/tcp/co_tcp/co_tcp_server/co_tcp_session_send.hpp"
#include "utils/stringex.hpp"
#include <sstream>
#include <vector>

namespace cpp_streamer
{
int GetUriAndParams(const std::string& input, std::string& uri, std::map<std::string, std::string>& params) {
    size_t pos = input.find("?");
    if (pos == input.npos) {
        uri = input;
        return 0;
    }
    uri = input.substr(0, pos);

    std::string param_str = input.substr(pos+1);
    std::vector<std::string> item_vec;

    StringSplit(param_str, "&", item_vec);

    for (auto& item : item_vec) {
        std::vector<std::string> param_vec;
        StringSplit(item, "=", param_vec);
        if (param_vec.size() != 2) {
            return -1;
        }
        params[param_vec[0]] =param_vec[1];
    }

    return 0;
}


std::string GetUri(std::string& uri) {
    if (uri == "/") {
        return uri;
    }
    size_t pos = uri.find("/");
    if (pos == 0) {
        uri = uri.substr(1);
    }
    pos = uri.rfind("/");
    if (pos == (uri.length() - 1)) {
        uri = uri.substr(0, pos);
    }

    return uri;
}

CoHttpRequest::CoHttpRequest()
{
    header_data_ = std::make_shared<DataBuffer>();
    content_data_ = std::make_shared<DataBuffer>();
}

CoHttpRequest::~CoHttpRequest()
{
}

std::string CoHttpRequest::Dump() {
    std::stringstream ss;
    ss << "Method: " << method_ << "\n";
    ss << "URI: " << uri_ << "\n";
    ss << "Version: " << version_ << "\n";
    ss << "Content-Length: " << content_length_ << "\n";
    ss << "Headers:\n";
    for (const auto& header : headers_) {
        ss << header.first << ": " << header.second << "\n";
    }
    if (!params.empty()) {
        ss << "Params:\n";
        for (const auto& param : params) {
            ss << param.first << ": " << param.second << "\n";
        }
    }

    if (content_length_ > 0 && content_data_ && content_data_->DataLen() > 0) {
        ss << "Content Data: " << std::string(content_data_->Data(), content_data_->DataLen()) << "\n";
    }
    return ss.str();
}
CoHttpResponse::CoHttpResponse(std::shared_ptr<TcpCoAcceptConn> accept_conn, HttpResponseCallbackI* cb, Logger* logger)
    : accept_conn_(accept_conn), logger_(logger), cb_(cb)
{
}

CoHttpResponse::~CoHttpResponse()
{
    Close();
}
CoTask<SendResult> CoHttpResponse::WriteStatus(uint32_t code, const std::string& status, bool continue_flag) {
	if (is_close_ || accept_conn_ == nullptr) {
		LogErrorf(logger_, "Cannot write to closed response");
		co_return SendResult{ SEND_ERROR, -1 };
	}

	status_ = status;
	std::stringstream ss;
	ss << proto_ << "/" << version_ << " " << code << " " << status_ << "\r\n";
	ss << "Content-Length: 0\r\n";
	ss << "\r\n";
	std::string header_str = ss.str();
	int header_len = (int)header_str.length();
	uint8_t* p = reinterpret_cast<uint8_t*>(const_cast<char*>(header_str.c_str()));
	do {
        SendResult send_result = co_await accept_conn_->CoSend(p, header_len);

		if (send_result.status != SEND_OK) {
			LogErrorf(logger_, "Failed to send HTTP response status, status: %d", send_result.status);
			co_return send_result;
		}
		p += send_result.sent_len;
		header_len -= send_result.sent_len;
	} while (header_len > 0);
	LogInfof(logger_, "HTTP response status sent:%s", header_str.c_str());
    written_header_ = true;
    if (!continue_flag) {
        is_close_ = true;
        cb_->OnResponseClose();
    }
	co_return SendResult{
		.status = SEND_OK,
		.sent_len = static_cast<int>(header_str.length())
	};
}
CoTask<SendResult> CoHttpResponse::Write(const char* data, size_t len, bool continue_flag)
{
    std::vector<uint8_t> send_data(data, data + len);

    if (is_close_ || accept_conn_ == nullptr) {
        LogErrorf(logger_, "Cannot write to closed response");
        co_return SendResult{SEND_ERROR, -1};
    }

    if (!written_header_) {
        std::stringstream ss;
        ss << proto_ << "/" << version_ << " " << status_code_ << " " << status_ << "\r\n";
        for (const auto& header : headers_) {
            ss << header.first << ": " << header.second << "\r\n";
        }
        if (!continue_flag) {
            ss << "Content-Length: " << len << "\r\n";
        }
        ss << "\r\n";

        std::string header_str = ss.str();
        int header_len = (int)header_str.length();
        uint8_t* p = reinterpret_cast<uint8_t*>(const_cast<char*>(header_str.c_str()));
        
        do {
			LogInfof(logger_, "Sending HTTP response header: %s", header_str.c_str());
			// Send the header
            SendResult send_result = co_await accept_conn_->CoSend(p, header_len);
            if (send_result.status != SEND_OK) {
                LogErrorf(logger_, "Failed to send HTTP response header, status: %d", send_result.status);
                co_return send_result;
            }
            cb_->OnResponseAlive();
            p += send_result.sent_len;
            header_len -= send_result.sent_len;
        } while (header_len > 0);

        written_header_ = true;
    }

    int total_len = (int)len;
    uint8_t* p = &send_data[0];

    do {
		LogInfof(logger_, "Sending HTTP response body, remaining length: %d", total_len);
        SendResult send_result = co_await accept_conn_->CoSend(p, total_len);
        if (send_result.status != SEND_OK) {
            LogErrorf(logger_, "Failed to send data, status: %d", send_result.status);
            co_return send_result;
        }
        cb_->OnResponseAlive();
        p += send_result.sent_len;
        total_len -= send_result.sent_len;
    } while(total_len > 0);

    if (!continue_flag) {
        is_close_ = true;
		LogInfof(logger_, "HTTP response body sent, closing response.");
        cb_->OnResponseClose();
    }
    co_return SendResult {
        .status = SEND_OK,
        .sent_len = static_cast<int>(len)
    };
}

void CoHttpResponse::Close()
{
    if (is_close_) {
        return;
    }
    is_close_ = true;
    accept_conn_ = nullptr;
}

} // namespace cpp_streamer