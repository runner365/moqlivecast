#include "stun.hpp"
#include "byte_stream.hpp"
#include "byte_crypto.hpp"
#include "logger.hpp"
#include "ipaddress.hpp"

#include <stdint.h>
#include <memory>
#include <cstring>
#include <iostream>
#include <sstream>
#include <stdio.h>

namespace cpp_streamer
{
//The magic cookie field MUST contain the fixed value 0x2112A442
const uint8_t StunPacket::magic_cookie[] = { 0x21, 0x12, 0xA4, 0x42 };

StunPacket* StunPacket::Parse(uint8_t* data, size_t len) {
    if (!StunPacket::IsStun(data, len)) {
        throw CppStreamException("it's not a stun packet");
    }

/*
   The message type field is decomposed further into the following
   structure:
                 0                 1
                 2  3  4 5 6 7 8 9 0 1 2 3 4 5

                +--+--+-+-+-+-+-+-+-+-+-+-+-+-+
                |M |M |M|M|M|C|M|M|M|C|M|M|M|M|
                |11|10|9|8|7|1|6|5|4|0|3|2|1|0|
                +--+--+-+-+-+-+-+-+-+-+-+-+-+-+

                Figure 3: Format of STUN Message Type Field
*/
    uint8_t* p = data;
    uint16_t msg_type = ByteStream::Read2Bytes(p);
    p += 2;
    uint16_t msg_len  = ByteStream::Read2Bytes(p);
    p += 2;

    if (((size_t)msg_len != (len - 20)) || ((msg_len & 0x03) != 0)) {
        CSM_THROW_ERROR("stun packet message len(%d) error, len:%zu", msg_len, len);
    }

    StunPacket* ret_packet = new StunPacket(data, len);

    uint16_t msg_method = (msg_type & 0x000f) | ((msg_type & 0x00e0) >> 1) | ((msg_type & 0x3E00) >> 2);
    ret_packet->stun_method_ = static_cast<STUN_METHOD_ENUM>(msg_method);

    uint16_t msg_class = ((data[0] & 0x01) << 1) | ((data[1] & 0x10) >> 4);
    ret_packet->stun_class_  = static_cast<STUN_CLASS_ENUM>(msg_class);

    p += 4;//skip Magic Cookie(4bytes)
    ret_packet->transaction_id_ = p;
    p += 12;//skip Transaction ID (12bytes)
    
    bool has_fingerprint = false;
    bool has_message_integrity = false;
    size_t fingerprint_pos   = 0;

    while ((p + 4) < (data + len)) {
        STUN_ATTRIBUTE_ENUM attr_type = static_cast<STUN_ATTRIBUTE_ENUM>(ByteStream::Read2Bytes(p));
        p += 2;
        uint16_t attr_len = ByteStream::Read2Bytes(p);
        p += 2;

        if ((p + attr_len) > (data + len)) {
            delete ret_packet;
            CSM_THROW_ERROR("stun packet attribute length(%d) is too long", attr_len);
        }

        if (has_fingerprint) {
            delete ret_packet;
            CSM_THROW_ERROR("stun packet attribute fingerprint must be the last one");
        }

        if (has_message_integrity && (attr_type != STUN_FINGERPRINT))
        {
            delete ret_packet;
            CSM_THROW_ERROR("fingerprint is only allowed after message integrity attribute.");
        }

        const uint8_t* attr_data = p;
        size_t skip_len = (size_t)ByteStream::PadTo4Bytes((uint16_t)(attr_len));
        p += skip_len;

        switch (attr_type)
        {
            case STUN_USERNAME:
            {
                ret_packet->username_.assign((char*)attr_data, attr_len);
                break;
            }
            case STUN_PRIORITY:
            {
                if (attr_len != 4) {
                    delete ret_packet;
                    CSM_THROW_ERROR("stun attribute priority len(%d) is not 4", attr_len);
                }
                ret_packet->priority_ = ByteStream::Read4Bytes(attr_data);
                break;
            }
            case STUN_ICE_CONTROLLING:
            {
                if (attr_len != 8) {
                    delete ret_packet;
                    CSM_THROW_ERROR("stun attribute icecontrolling len(%d) is not 8", attr_len);
                }
                ret_packet->ice_controlling_ = ByteStream::Read8Bytes(attr_data);
                break;
            }
            case STUN_ICE_CONTROLLED:
            {
                if (attr_len != 8) {
                    delete ret_packet;
                    CSM_THROW_ERROR("stun attribute icecontrolled len(%d) is not 8", attr_len);
                }
                ret_packet->ice_controlled_ = ByteStream::Read8Bytes(attr_data);
                break;
            }
            case STUN_USE_CANDIDATE:
            {
                if (attr_len != 0) {
                    delete ret_packet;
                    CSM_THROW_ERROR("stun attribute use candidate len(%d) is not 0", attr_len);
                }
                ret_packet->has_use_candidate_ = true;
                break;
            }
            case STUN_MESSAGE_INTEGRITY:
            {
                if (attr_len != 20) {
                    delete ret_packet;
                    CSM_THROW_ERROR("stun attribute message integrity len(%d) is not 20", attr_len);
                }
                has_message_integrity = true;
                ret_packet->message_integrity_ = attr_data;
                break;
            }
            case STUN_FINGERPRINT:
            {
                if (attr_len != 4) {
                    delete ret_packet;
                    CSM_THROW_ERROR("stun attribute fingerprint len(%d) is not 4", attr_len);
                }
                has_fingerprint = true;
                ret_packet->fingerprint_ = ByteStream::Read4Bytes(attr_data);
                fingerprint_pos = (attr_data - 4) - data;
                break;
            }
            case STUN_ERROR_CODE:
            {
                if (attr_len != 4) {
                    delete ret_packet;
                    CSM_THROW_ERROR("stun attribute error code len(%d) is not 4", attr_len);
                }
                attr_data += 2;
                uint8_t error_class = *attr_data;
                attr_data++;
                uint8_t error_number = *attr_data;
                ret_packet->error_code_ = (uint16_t)(error_class * 100 + error_number);
                break;
            }
            case STUN_XOR_MAPPED_ADDRESS:
            {
                if (attr_len != 8) {
                    delete ret_packet;
                    CSM_THROW_ERROR("stun attribute xor mapped address len(%d) is not 8", attr_len);
                }
                uint8_t ip_data[8];
                memcpy(ip_data, attr_data, sizeof(ip_data));
                uint16_t net_family = ByteStream::Read2Bytes(ip_data);
                ip_data[2] ^= StunPacket::magic_cookie[0];
                ip_data[3] ^= StunPacket::magic_cookie[1];

                ret_packet->xor_address_ = (struct sockaddr*)malloc(sizeof(struct sockaddr));
                ret_packet->xor_address_allocated_ = true;
                ((struct sockaddr_in*)(ret_packet->xor_address_))->sin_family = (net_family == 1) ? AF_INET : AF_INET6;
                ((struct sockaddr_in*)(ret_packet->xor_address_))->sin_port = ntohs(ByteStream::Read2Bytes(&ip_data[2]));

                ip_data[4] ^= StunPacket::magic_cookie[0];
                ip_data[5] ^= StunPacket::magic_cookie[1];
                ip_data[6] ^= StunPacket::magic_cookie[2];
                ip_data[7] ^= StunPacket::magic_cookie[3];

                ((struct sockaddr_in*)(ret_packet->xor_address_))->sin_addr.s_addr = ntohl(ByteStream::Read4Bytes(&ip_data[4]));
                break;
            }
            default:
            {
            }
        }
    }
    
    if (p != (data + len)) {
        delete ret_packet;
        CSM_THROW_ERROR("data offset(%p) is not data end(%p)", p, data + len);
    }
/*
15.5.  FINGERPRINT
   The FINGERPRINT attribute MAY be present in all STUN messages.  The
   value of the attribute is computed as the CRC-32 of the STUN message
   up to (but excluding) the FINGERPRINT attribute itself, XOR'ed with
   the 32-bit value 0x5354554e (the XOR helps in cases where an
   application packet is also using CRC-32 in it)
*/
    if (has_fingerprint) {
        uint32_t caculate_fingerprint = ByteCrypto::GetCrc32(data, fingerprint_pos);
        caculate_fingerprint ^= 0x5354554e;
        if (ret_packet->fingerprint_ != caculate_fingerprint) {
            delete ret_packet;
            CSM_THROW_ERROR("fingerprint(%u) is not equal caculate fingerprint(%u)",
                ret_packet->fingerprint_, caculate_fingerprint);
        }
        ret_packet->has_fingerprint_ = true;
    }
    return ret_packet;
}

/*
rfc: https://datatracker.ietf.org/doc/html/rfc5389
All STUN messages MUST start with a 20-byte header followed by zero
or more Attributes.  The STUN header contains a STUN message type,
magic cookie, transaction ID, and message length.
    0                   1                   2                   3
    0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
   |0 0|     STUN Message Type     |         Message Length        |
   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
   |                         Magic Cookie                          |
   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
   |                                                               |
   |                     Transaction ID (96 bits)                  |
   |                                                               |
   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
*/
StunPacket::StunPacket()
{
    data_ = new uint8_t[8192];
    memset(data_, 0, 8192);
    data_len_ = 0;
    need_delete_ = true;
}

StunPacket::StunPacket(uint8_t* data, size_t len) {
    data_ = data;
    data_len_ = len;
    need_delete_ = false;
}

StunPacket::~StunPacket()
{
    if (xor_address_ && xor_address_allocated_) {
        free(xor_address_);
        xor_address_ = nullptr;
    }
    if (need_delete_ && data_) {
        delete[] data_;
        data_ = nullptr;
    }
}

bool StunPacket::IsStun(const uint8_t* data, size_t len) {
    if ((len >= STUN_HEADER_SIZE) && (data[0] < 3) &&
        (data[4] == StunPacket::magic_cookie[0]) && (data[5] == StunPacket::magic_cookie[1]) &&
        (data[6] == StunPacket::magic_cookie[2]) && (data[7] == StunPacket::magic_cookie[3])) {
        return true;
    }

    return false;
}

bool StunPacket::IsBindingRequest(const uint8_t *buf, size_t buf_size) {
    if (!IsStun(buf, buf_size)) {
        return false;
    }
    return buf_size > 1 && buf[0] == 0x00 && buf[1] == 0x01;
}

bool StunPacket::IsBindingResponse(const uint8_t *buf, size_t buf_size) {
    if (!IsStun(buf, buf_size)) {
        return false;
    }
    return buf_size > 1 && buf[0] == 0x01 && buf[1] == 0x01;
}

/*
need to initialize:
1) username_: 
snprintf(username, sizeof(username), "%s:%s",
      dtls->remote_fragment_, rtc->local_fragment_);
2) add_msg_integrity_, dtls->remote_pwd_ for password_;
ByteCrypto::GetHmacSha1(password_,...


*/
int StunPacket::Serialize() {
    add_msg_integrity_ = (stun_class_ != STUN_CLASS_ENUM::STUN_ERROR_RESPONSE && !password_.empty());
    
    uint16_t user_name_pad_len = ByteStream::PadTo4Bytes((uint16_t)(username_.length()));
    data_len_ = STUN_HEADER_SIZE;//init stun data size

    if (!username_.empty()) {
        data_len_ += 4 + user_name_pad_len;
    }
    
    if (priority_) {
        data_len_ += 4 + 4;
    }

    if (has_use_candidate_) {
        data_len_ += 4;
    }
    if (xor_address_) {
        data_len_ += 4 + 8;
    }
    if (add_msg_integrity_) {
        data_len_ += 4 + 20;
    }

    //add fingerprint always
    data_len_ += 4 + 4;
    
    uint8_t* p = data_;

	uint16_t type_field = (static_cast<uint16_t>(stun_method_) & 0x0f80) << 2;

	type_field |= (static_cast<uint16_t>(stun_method_) & 0x0070) << 1;
	type_field |= (static_cast<uint16_t>(stun_method_) & 0x000f);
	type_field |= (static_cast<uint16_t>(stun_class_) & 0x02) << 7;
	type_field |= (static_cast<uint16_t>(stun_class_) & 0x01) << 4;

    ByteStream::Write2Bytes(p, type_field);
    p += 2;
    ByteStream::Write2Bytes(p, static_cast<uint16_t>(data_len_) - 20);
    p += 2;

    //wait to write data len
    memcpy(p, StunPacket::magic_cookie, 4);
    p += 4;
    memcpy(p, transaction_id_, 12);
    p += 12;

    //start set attributes
    if (!username_.empty()) {
        ByteStream::Write2Bytes(p, (uint16_t)STUN_USERNAME);
        p += 2;
        ByteStream::Write2Bytes(p, (uint16_t)(username_.length()));
        p += 2;
        memcpy(p, username_.c_str(), username_.length());
        p += user_name_pad_len;
    }

    //set priority
    if (priority_) {
        ByteStream::Write2Bytes(p, (uint16_t)STUN_PRIORITY);
        p += 2;
        ByteStream::Write2Bytes(p, 4);
        p += 2;
        ByteStream::Write4Bytes(p, priority_);
        p += 4;
    }

    if (has_use_candidate_) {
        ByteStream::Write2Bytes(p, (uint16_t)STUN_USE_CANDIDATE);
        p += 2;
        ByteStream::Write2Bytes(p, 0);
        p += 2;
    }
    if (xor_address_) {
        ByteStream::Write2Bytes(p, (uint16_t)STUN_XOR_MAPPED_ADDRESS);
        p += 2;
        ByteStream::Write2Bytes(p, 8);
        p += 2;

        uint16_t net_family = 0x01;//only support ipv4 now
        ByteStream::Write2Bytes(p, net_family);
        p += 2;

        uint16_t port = 0;
        std::string ip_str = GetIpStr(xor_address_, port);
        uint16_t xored_port = port ^ ((StunPacket::magic_cookie[0] << 8) | StunPacket::magic_cookie[1]);
        ByteStream::Write2Bytes(p, xored_port);
        p += 2;

        uint32_t ip_addr = IpStringToUint32(ip_str);
        uint32_t xored_ip = ip_addr ^ ByteStream::Read4Bytes(StunPacket::magic_cookie);
        ByteStream::Write4Bytes(p, xored_ip);
        p += 4;
    }
    if (add_msg_integrity_) {
        size_t pos = p - data_;

        //reset for ignore fingerprint
        //subtract message integrity and fingerprint part
        ByteStream::Write2Bytes(data_ + 2, (uint16_t)(data_len_ - 20 - 8));

        uint8_t* caculate_msg_integrity = ByteCrypto::GetHmacSha1(password_,
                                                            data_, pos);
        
        ByteStream::Write2Bytes(p, STUN_MESSAGE_INTEGRITY);
        p += 2;
        ByteStream::Write2Bytes(p, 20);
        p += 2;
        message_integrity_ = p;
        memcpy(p, caculate_msg_integrity, 20);
        p += 20;

        //recover the message length
        ByteStream::Write2Bytes(data_ + 2, (uint16_t)(data_len_ - STUN_HEADER_SIZE));
    } else {
        message_integrity_ = nullptr;
    }

    do {//set fingerprint
        size_t pos = p - data_;
        /* xor with "STUN" */
        uint32_t caculate_fingerprint = ByteCrypto::GetCrc32(data_, pos) ^ 0x5354554e;

        ByteStream::Write2Bytes(p, STUN_FINGERPRINT);
        p += 2;
        ByteStream::Write2Bytes(p, 4);
        p += 2;
        ByteStream::Write4Bytes(p, caculate_fingerprint);
        p += 4;
    } while(0);

    assert((size_t)(p - data_) == data_len_);
    return (int)data_len_;
}

std::string StunPacket::Dump() {
    std::stringstream ss;

    ss << "stun packet:\r\n";
    ss << "  class:" << this->stun_class_ << ", method:" << this->stun_method_ << "\r\n";
    ss << "  data length:" << this->data_len_ << "\r\n";

    char transactionid_sz[64];
    int str_len = 0;

    for (size_t i = 0; i < sizeof(this->transaction_id_); i++) {
        str_len += snprintf(transactionid_sz + str_len, sizeof(transactionid_sz) - str_len,
                        " %.2x", this->transaction_id_[i]);
    }
    ss << "  transaction id:" << transactionid_sz << "\r\n";
    
    ss << "  username:" << this->username_ << "\r\n";
    ss << "  priority:" << this->priority_ << "\r\n";
    ss << "  ice_controlling:" << this->ice_controlling_ << "\r\n";
    ss << "  ice_controlled:" << this->ice_controlled_ << "\r\n";
    ss << "  fingerprint:" << this->fingerprint_ << "\r\n";
    ss << "  error_code:" << this->error_code_ << "\r\n";

    if (this->message_integrity_) {
        char message_integrity_sz[64];
        str_len = 0;
        for (int i = 0; i < 20; i++) {
            str_len += snprintf(message_integrity_sz + str_len, 20 - str_len,
                        "%.2x", this->message_integrity_[i]);
        }
        ss << "  message_integrity:" << message_integrity_sz << "\r\n";
    }

    ss << "  has_use_candidate:" << this->has_use_candidate_ << "\r\n";

    if (this->xor_address_) {
        uint16_t port = 0;
        std::string ip_str = GetIpStr(this->xor_address_, port);
        ss << "  xor_address:" <<  (int)(this->xor_address_->sa_family) << " "<< ip_str << ":" << port << "\r\n";
    }
    return ss.str();
}

STUN_AUTHENTICATION StunPacket::CheckAuthentication(const std::string& ufrag, const std::string& pwd) {
    size_t user_name_len = ufrag.length();
    std::string pkt_user_name = username_.substr(0, user_name_len);

    if (pkt_user_name != ufrag) {
        return STUN_AUTHENTICATION::UNAUTHORIZED;
    }
	// If there is FINGERPRINT it must be discarded for MESSAGE-INTEGRITY calculation,
	// so the header length field must be modified (and later restored).
	if (has_fingerprint_) {
        ByteStream::Write2Bytes(this->data_ + 2, static_cast<uint16_t>(this->data_len_ - 20 - 8));
    }
    size_t hmac_len = (this->message_integrity_ - 4) - this->data_;
    const uint8_t*  computed_message_integrity = ByteCrypto::GetHmacSha1(pwd, 
      this->data_, hmac_len);
	if (std::memcmp(this->message_integrity_, computed_message_integrity, 20) != 0) {
        return STUN_AUTHENTICATION::UNAUTHORIZED;
    }

	// Restore the header length field.
	if (this->has_fingerprint_) {
        ByteStream::Write2Bytes(this->data_ + 2, static_cast<uint16_t>(this->data_len_ - STUN_HEADER_SIZE));
    }
    
    return STUN_AUTHENTICATION::OK;
}

StunPacket* StunPacket::CreateSuccessResponse() {
    StunPacket* resp_pkt = new StunPacket();

    resp_pkt->stun_class_ = STUN_CLASS_ENUM::STUN_SUCCESS_RESPONSE;
    resp_pkt->stun_method_ = this->stun_method_;
    resp_pkt->transaction_id_ = this->transaction_id_;

    return resp_pkt;
}
}
