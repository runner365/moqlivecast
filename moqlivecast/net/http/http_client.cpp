#include "http_client.hpp"
#include "utils/logger.hpp"
#include "utils/stringex.hpp"

#include <string>
#include <sstream>
#include <uv.h>
#include <assert.h>

namespace cpp_streamer
{

HttpClient::HttpClient(uv_loop_t* loop,
                       const std::string& host,
                       uint16_t port,
                       HttpClientCallbackI* cb,
                       Logger* logger,
                       bool ssl_enable): host_(host)
                                         , port_(port)
                                         , cb_(cb)
                                         , logger_(logger)
{
    client_ = new TcpClient(loop, this, logger_, ssl_enable);
}

HttpClient::~HttpClient()
{
    LogInfof(logger_, "HttpClient destruct...");
    if (client_) {
        delete client_;
        client_ = nullptr;
    }
}

int HttpClient::Get(const std::string& subpath, const std::map<std::string, std::string>& headers) {
    method_  = HTTP_GET;
    subpath_ = subpath;
    headers_ = headers;

    LogInfof(logger_, "http get connect host:%s, port:%d, subpath:%s", host_.c_str(), port_, subpath.c_str());
    client_->Connect(host_, port_);
    return 0;
}

int HttpClient::Post(const std::string& subpath, const std::map<std::string, std::string>& headers, const std::string& data) {
    method_    = HTTP_POST;
    subpath_   = subpath;
    post_data_ = data;
    headers_   = headers;

    client_->Connect(host_, port_);
    LogInfof(logger_, "http post connect host:%s, port:%d, subpath:%s, post data:%s", 
            host_.c_str(), port_, subpath.c_str(), data.c_str());
    return 0;
}

void HttpClient::Close() {
    LogInfof(logger_, "http close...");
    client_->Close();
}

TcpClient* HttpClient::GetTcpClient() {
    return client_;
}

void HttpClient::OnConnect(int ret_code) {
    if (ret_code < 0) {
        LogErrorf(logger_, "http client OnConnect error:%d", ret_code);
        std::shared_ptr<HttpClientResponse> resp_ptr;
        cb_->OnHttpRead(ret_code, resp_ptr);
        return;
    }
    std::stringstream http_stream;

    LogInfof(logger_, "on connect code:%d", ret_code);
    if (method_ == HTTP_GET) {
        http_stream << "GET " << subpath_ << " HTTP/1.1\r\n";
    } else if (method_ == HTTP_POST) {
        http_stream << "POST " << subpath_ << " HTTP/1.1\r\n";
    } else {
        CSM_THROW_ERROR("unkown http method:%d", method_);
    }
    http_stream << "Accept: */*\r\n";
    http_stream << "Host: " << host_ << "\r\n";
    for (auto& header : headers_) {
        http_stream << header.first << ": " << header.second << "\r\n";
    }
    if (method_ == HTTP_POST) {
        http_stream << "Content-Length: " << post_data_.length() << "\r\n";
    }
    http_stream << "\r\n";
    if (method_ == HTTP_POST) {
        http_stream << post_data_;
    }
    LogInfof(logger_, "http post:%s", http_stream.str().c_str());
    client_->Send(http_stream.str().c_str(), http_stream.str().length());
}

void HttpClient::OnWrite(int ret_code, size_t sent_size) {
    if (ret_code == 0) {
        client_->AsyncRead();
    }
}

void HttpClient::OnRead(int ret_code, const char* data, size_t data_size) {
    uint8_t* content_p = nullptr;
	size_t content_len = 0;

    if (ret_code < 0) {
        LogErrorf(logger_, "http client OnRead error:%d, err name:%s, err msg:%s", ret_code, uv_err_name(ret_code), uv_strerror(ret_code));
        cb_->OnHttpRead(ret_code, resp_ptr_);
        return;
    }
	
    if (data_size == 0) {
        cb_->OnHttpRead(-2, resp_ptr_);
        return;
    }

    if (!resp_ptr_) {
        resp_ptr_ = std::make_shared<HttpClientResponse>();
    }

    if (!resp_ptr_->header_ready_) {
		header_buffer_.AppendData(data, data_size);

        std::string header_str(header_buffer_.Data(), header_buffer_.DataLen());
        size_t pos = header_str.find("\r\n\r\n");
        if (pos != std::string::npos) {
            std::vector<std::string> lines_vec;
            resp_ptr_->header_ready_ = true;
            header_str = header_str.substr(0, pos);
            content_p = (uint8_t*)data + pos + 4;
			content_len = data_size - (pos + 4);

            StringSplit(header_str, "\r\n", lines_vec);
            for (size_t i = 0; i < lines_vec.size(); i++) {
                if (i == 0) {
                    std::vector<std::string> item_vec;
                    StringSplit(lines_vec[0], " ", item_vec);
                    assert(item_vec.size() >= 3);

                    pos = item_vec[0].find("/");
                    assert(pos != std::string::npos);
                    resp_ptr_->proto_   = item_vec[0].substr(0, pos);
                    resp_ptr_->version_ = item_vec[0].substr(pos+1);
                    resp_ptr_->status_code_ = atoi(item_vec[1].c_str());
                    if (item_vec.size() == 3) {
                        resp_ptr_->status_      = item_vec[2];
                    } else {
                        std::string status_string("");
                        for (size_t i = 2; i < item_vec.size(); i++) {
                            status_string += item_vec[i];
                        }
                        resp_ptr_->status_ = status_string;
                    }
                    continue;
                }
                pos = lines_vec[i].find(":");
                assert(pos != std::string::npos);
                std::string key   = lines_vec[i].substr(0, pos);
                std::string value = lines_vec[i].substr(pos + 2);

                if (key == "Content-Length") {
                    resp_ptr_->content_length_ = atoi(value.c_str());
                    LogInfof(logger_, "http content length:%d", resp_ptr_->content_length_);
                }
                if (key == "Transfer-Encoding" && value == "chunked") {
                    resp_ptr_->chunked_ = true;
                }
                resp_ptr_->headers_[key] = value;
                LogInfof(logger_, "header: %s: %s", key.c_str(), value.c_str());
            }
            if (!resp_ptr_->chunked_) {
                resp_ptr_->data_.AppendData((char*)content_p, content_len);
			}
        } else {
            LogInfof(logger_, "header not ready, read more");
            client_->AsyncRead();
            return;
        }
    }
    else {
        if (!resp_ptr_->chunked_) {
			resp_ptr_->data_.AppendData(data, data_size);
        }
        else {
			OnHandleChuckedBody((uint8_t*)data, data_size);
            return;
        }
    }
    if (resp_ptr_->chunked_) {
        OnHandleChuckedBody(content_p, content_len);
        return;
	}

    if (resp_ptr_->content_length_ > 0) {
        LogInfof(logger_, "http receive data len:%lu, content len:%d",
                resp_ptr_->data_.DataLen(), resp_ptr_->content_length_);
        if ((int)resp_ptr_->data_.DataLen() >= resp_ptr_->content_length_) {
            resp_ptr_->body_ready_ = true;
            cb_->OnHttpRead(0, resp_ptr_);
        } else {
            client_->AsyncRead();
        }
    } else {
        cb_->OnHttpRead(0, resp_ptr_);
        client_->AsyncRead();
    }
}

void HttpClient::OnHandleChuckedBody(uint8_t* data, size_t data_len) {
    if (!resp_ptr_->chunked_) {
        return;
    }
	uint8_t* p = data;
	int64_t content_len = (int64_t)data_len;

    while (true) {
        std::string body_str((char*)p, content_len);
        size_t pos = body_str.find("\r\n");
        if (pos == std::string::npos) {
            LogInfof(logger_, "chunked body not ready, read more");
            client_->AsyncRead();
            return;
        }
        std::string chunk_size_str = body_str.substr(0, pos);
        char* endptr = nullptr;
        int chunk_size = (int)strtol(chunk_size_str.c_str(), &endptr, 16);
        // 检查是否有合法数字被解析
        if (endptr == chunk_size_str.c_str() || chunk_size < 0) {
            break;
        }
        if (chunk_size == 0) {
            resp_ptr_->body_ready_ = true;
            cb_->OnHttpRead(0, resp_ptr_);
            return;
        }
		p += pos + 2;
		content_len -= (int64_t)(pos + 2);

		resp_ptr_->data_.AppendData((char*)p, (size_t)chunk_size);

		std::string content_str(resp_ptr_->data_.Data(), resp_ptr_->data_.DataLen());
		LogInfof(logger_, "chunked body size:%d, content:%s", chunk_size, content_str.c_str());
        // resp_ptr_->data_.ConsumeData(chunk_size);
        // resp_ptr_->data_.ConsumeData(2); //\r\n
	}
}
}
