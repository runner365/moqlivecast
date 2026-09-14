#include "websocket_client.hpp"
#include "websocket_pub.hpp"
#include "utils/stringex.hpp"
#include "utils/byte_crypto.hpp"
#include "utils/base64.hpp"
#include "utils/byte_stream.hpp"
#include <sstream>

namespace cpp_streamer
{
#define WebSocket_Key_Len 16
#define Websocket_Response_Code 101
#define Websocket_Key_Hash "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
#define kWebSocketPingInterval 1000
#define kWebSocketPongTimeout (15*1000)

WebSocketClient::WebSocketClient(uv_loop_t* loop, 
                                const std::string& hostname, 
                                uint16_t port, 
                                const std::string& subpath, 
                                bool ssl_enable, 
                                Logger* logger,
                                WebSocketConnectionCallBackI* conn_cb):TimerInterface(200)
                                                                    , WebSocketSessionBase(logger)
                                                                    , loop_(loop)
                                                                    , hostname_(hostname)
                                                                    , port_(port)
                                                                    , subpath_(subpath)
                                                                    , ssl_enable_(ssl_enable)
                                                                    , logger_(logger)
                                                                    , conn_cb_(conn_cb)
{
    client_ptr_.reset(new HttpClient(loop_, hostname_, port_, this, logger_, ssl_enable_));
    frame_.reset(new WebSocketFrame());
    key_ = ByteCrypto::GetRandomString(WebSocket_Key_Len);

    StartTimer();
    LogInfof(logger_, "WebSocketClient construct, hostname:%s, port:%d, sugpath:%s, https:%s",
        hostname_.c_str(), port_, subpath_.c_str(), BOOL2STRING(ssl_enable_));
}

WebSocketClient::~WebSocketClient()
{
    LogInfof(logger_, "WebSocketClient destruct, hostname:%s, port:%d, sugpath:%s, https:%s",
        hostname_.c_str(), port_, subpath_.c_str(), BOOL2STRING(ssl_enable_));
}

void WebSocketClient::AsyncConnect(const std::map<std::string, std::string>& input_headers) {
    std::map<std::string, std::string> headers;
    headers["Upgrade"]                  = "websocket";
    headers["Connection"]               = "Upgrade";
    headers["Sec-WebSocket-Key"]        = Base64Encode((uint8_t*)key_.c_str(), WebSocket_Key_Len);
    headers["Sec-WebSocket-Version"]    = std::to_string(13);

    for (const auto& it : input_headers) {
        headers[it.first] = it.second;
    }
    client_ptr_->Get(subpath_, headers);
}

void WebSocketClient::SendWsFrame(const uint8_t* data, size_t len, uint8_t op_code) {
    WS_PACKET_HEADER* ws_header;
    uint8_t header_start[WS_MAX_HEADER_LEN];
    size_t header_len = 2;

    ws_header = (WS_PACKET_HEADER*)header_start;
    memset(header_start, 0, WS_MAX_HEADER_LEN);
    ws_header->fin      = 1;
    ws_header->opcode   = op_code;

    if (len >= 126) {
        if (len > UINT16_MAX) {
            ws_header->payload_len = 127;
			ws_header->payload_len = 127;
			*(uint8_t*)(header_start + 2) = (len >> 56) & 0xFF;
			*(uint8_t*)(header_start + 3) = (len >> 48) & 0xFF;
			*(uint8_t*)(header_start + 4) = (len >> 40) & 0xFF;
			*(uint8_t*)(header_start + 5) = (len >> 32) & 0xFF;
			*(uint8_t*)(header_start + 6) = (len >> 24) & 0xFF;
			*(uint8_t*)(header_start + 7) = (len >> 16) & 0xFF;
			*(uint8_t*)(header_start + 8) = (len >> 8) & 0xFF;
			*(uint8_t*)(header_start + 9) = (len >> 0) & 0xFF;
            header_len = WS_MAX_HEADER_LEN;
        } else {
			ws_header->payload_len = 126;
			*(uint8_t*)(header_start + 2) = (len >> 8) & 0xFF;
			*(uint8_t*)(header_start + 3) = (len >> 0) & 0xFF;
            header_len = 4;
        }
    } else {
        ws_header->payload_len = len;
        header_len = 2;
    }
    ws_header->mask = 1;

    uint8_t masking_key[4];

    masking_key[0] = ByteCrypto::GetRandomUint(1, 0xff);
    masking_key[1] = ByteCrypto::GetRandomUint(1, 0xff);
    masking_key[2] = ByteCrypto::GetRandomUint(1, 0xff);
    masking_key[3] = ByteCrypto::GetRandomUint(1, 0xff);
    
    std::vector<uint8_t> data_vec(len);
    uint8_t* p = &data_vec[0];
    
    memcpy(p, data, len);

    size_t temp_len = len & ~3;
    for (size_t i = 0; i < temp_len; i += 4) {
        p[i + 0] ^= masking_key[0];
        p[i + 1] ^= masking_key[1];
        p[i + 2] ^= masking_key[2];
        p[i + 3] ^= masking_key[3];
    }
    for (size_t i = temp_len; i < len; ++i) {
        p[i] ^= masking_key[i % 4];
    }
    size_t total = header_len + sizeof(masking_key) + len;
    std::vector<uint8_t> data_buffer(total);
    uint8_t* buffer_p = (uint8_t*)&data_buffer[0];

    memcpy(buffer_p, header_start, header_len);
    memcpy(buffer_p + header_len, masking_key, sizeof(masking_key));
    memcpy(buffer_p + header_len + sizeof(masking_key), p, len);

    client_ptr_->GetTcpClient()->Send((char*)buffer_p, total);
}

bool WebSocketClient::OnTimer() {
    if (!is_connected_) {
        return timer_running_;
    }
    int64_t now_ms = now_millisec();

    if (now_ms - last_send_ping_ms_ > 2000) {
        last_send_ping_ms_ = now_ms;
        SendPingFrame(now_ms);
    }

    if (last_recv_pong_ms_ <= 0) {
        last_recv_pong_ms_ = now_ms;
    } else {
        if (now_ms - last_recv_pong_ms_ > kWebSocketPongTimeout) {
            LogInfof(logger_, "ping/pong timeout, now:%ld, last:%ld", now_ms, last_recv_pong_ms_);
            is_connected_ = false;
            client_ptr_->GetTcpClient()->Close();
            conn_cb_->OnClose(-1, "ping/pong timeout");
        }
    }
    return timer_running_;
}

// status:SwitchingProtocols, code:101, headers:{Connection:  Upgrade,Sec-WebSocket-Accept:  xQweU7ZOYVoTNEk7/b3FhnLqIu4=,Upgrade:  websocket,}
void WebSocketClient::OnHttpRead(int ret, std::shared_ptr<HttpClientResponse> resp_ptr) {
    if (ret < 0) {
        is_connected_ = false;
        conn_cb_->OnClose(-1, "http read error");
        return;
    }
    if (!http_ready_) {
        HandleHttpRespone(resp_ptr);
    } else {
        HandleFrame(resp_ptr->data_);
        resp_ptr->data_.Reset();
    }
}

void WebSocketClient::HandleHttpRespone(std::shared_ptr<HttpClientResponse> resp_ptr) {
    std::stringstream ss;
    ss << "{";
    for (auto iter = resp_ptr->headers_.begin(); iter != resp_ptr->headers_.end(); iter++) {
        ss << " " << iter->first << ":" << iter->second;
    }
    ss << "}";
    LogInfof(logger_, "http response, status:%s, code:%d, headers:%s",
        resp_ptr->status_.c_str(), resp_ptr->status_code_, ss.str().c_str());
    ss.clear();
    try {
        if (resp_ptr->status_code_ != Websocket_Response_Code) {
            ss << "websocket http response code error:" << resp_ptr->status_code_;
            LogErrorf(logger_, "websocket http response code error(%d)", resp_ptr->status_code_);
            is_connected_ = false;
            conn_cb_->OnClose(-1, ss.str());
            return;
        }

        std::string connection_desc = resp_ptr->headers_["Connection"];
        if (connection_desc != "Upgrade") {
            ss << "websocket http response Connection error:" << connection_desc;
            LogErrorf(logger_, "websocket http response Connection error(%s)", connection_desc.c_str());
            is_connected_ = false;
            conn_cb_->OnClose(-1, ss.str());
            return;
        }
        std::string upgrade_desc = resp_ptr->headers_["Upgrade"];
        if (upgrade_desc != "websocket") {
            ss << "websocket http response Upgrade error:" << connection_desc;
            LogErrorf(logger_, "websocket http response Upgrade error(%s)", connection_desc.c_str());
            is_connected_ = false;
            conn_cb_->OnClose(-1, ss.str());
            return;
        }

        std::string hash_code = GenWebSocketHashcode(Base64Encode((uint8_t*)key_.c_str(), (unsigned int)key_.length()));
        std::string accept_hash = resp_ptr->headers_["Sec-WebSocket-Accept"];
        if (hash_code != accept_hash) {
            ss << "websocket http response Sec-WebSocket-Accept error, response:" << accept_hash << ", local hash:" << hash_code;
            LogErrorf(logger_, "websocket http response Sec-WebSocket-Accept error, response:%s, local hash:%s", accept_hash.c_str(), hash_code.c_str());
            is_connected_ = false;
            conn_cb_->OnClose(-1, ss.str());
            return;
        }
        LogInfof(logger_, "websocket http handshake ok");
        conn_cb_->OnConnection();
        http_ready_ = true;
        is_connected_ = true;
        resp_ptr->data_.Reset();
    } catch(const std::exception& e) {
        std::stringstream excepion_ss;
        excepion_ss << "websocket http handshake exception:" <<  e.what();
        LogErrorf(logger_, "websocket http handshake exception:%s", e.what());
        is_connected_ = false;
        conn_cb_->OnClose(-1, excepion_ss.str());
    }
}

void WebSocketClient::HandleWsData(uint8_t* data, size_t len, int op_code) {
    if (conn_cb_) {
        if (op_code == WS_OP_TEXT_TYPE) {
            conn_cb_->OnReadText(0, std::string((char*)data, len));
        } else if (op_code == WS_OP_BIN_TYPE) {
            conn_cb_->OnReadData(0, data, len);
        } else {
            LogErrorf(logger_, "handle unknown websocket data op_code:%d", op_code);
        }
    }
}

void WebSocketClient::HandleWsClose(uint8_t* data, size_t len) {
    if (close_) {
        return;
    }

    if (len <= 1) {
        SendClose(1002, "Incomplete close code");
    } else {
		bool invalid = false;
		uint16_t code = (uint8_t(data[0]) << 8) | uint8_t(data[1]);
		if(code < 1000 || code >= 5000) {
            invalid = true;
        }
		
		switch(code){
		    case 1004:
		    case 1005:
		    case 1006:
		    case 1015:
		    	invalid = true;
		    default:;
		}
		
		if(invalid){
			SendClose(1002, "Invalid close code");
		} else {
            SendWsFrame(data, len, WS_OP_CLOSE_TYPE);
        }
    }

    close_ = true;
    
    if (conn_cb_) {
        conn_cb_->OnClose(0, "close");
    }
    is_connected_ = false;
    if (client_ptr_->GetTcpClient()) {
        client_ptr_->GetTcpClient()->Close();
    }
}

}