#ifndef STRING_EXTEN_HPP
#define STRING_EXTEN_HPP
#include <random>
#include <string>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <algorithm>
#include <locale>
#include <codecvt>
#include <stdexcept>

namespace cpp_streamer
{

#define BOOL2STRING(x) ((x) ? "true" : "false")

inline int StringSplit(const std::string& input_str, const std::string& split_str, std::vector<std::string>& output_vec) {
    if (input_str.length() == 0) {
        return 0;
    }
    
    std::string tempString(input_str);
    do {

        size_t pos = tempString.find(split_str);
        if (pos == tempString.npos) {
            output_vec.push_back(tempString);
            break;
        }
        std::string seg_str = tempString.substr(0, pos);
        tempString = tempString.substr(pos+split_str.size());
        output_vec.push_back(seg_str);
    } while(tempString.size() > 0);

    return (int)output_vec.size();
}

inline std::string DataToString(char* data, size_t len) {
    char print_data[4*1024];
    size_t str_len = 0;

    size_t total = sizeof(print_data);
    for (size_t i = 0; i < len; i++) {
        str_len += snprintf(print_data + str_len, total - str_len,  "%c", data[i]);
    }
    return print_data;
}

inline std::string DataToString(uint8_t* data, size_t len, bool return_flag = true) {
    char print_data[4*1024];
    size_t print_len = 0;
    const int max_print = 512;

    for (size_t index = 0; index < (len > max_print ? max_print : len); index++) {        
        print_len += snprintf(print_data + print_len, sizeof(print_data) - print_len,
            " %02x", data[index]);
        if (return_flag) {
            if ((++index%16) == 0) {
                print_len += snprintf(print_data + print_len, sizeof(print_data) - print_len, "\r\n");
            }
        }
    }
    return std::string(print_data);
}

inline std::string Data2HexString(uint8_t* data, size_t len) {
    const size_t data_size = len * 4;
    std::vector<char> c0c1_desc(data_size);
    char* desc = (char*)(&c0c1_desc[0]);
    int desc_len = 0;

    for (size_t index = 0; index < len; index++) {
        desc_len += snprintf(desc + desc_len, data_size - desc_len, "%02x ", data[index]);
    }

    return std::string(desc);
}

inline std::string Uint32ToString(uint32_t value) {
    std::string ret = "";
    char c = (char)((value >> 24) & 0xff);
    ret += ((c != 0) ? c : ' ');
    c = (char)((value >> 16) & 0xff);
    ret += ((c != 0) ? c : ' ');
    c = (char)((value >> 8) & 0xff);
    ret += ((c != 0) ? c : ' ');
    c = (char)(value & 0xff);
    ret += ((c != 0) ? c : ' ');
    return ret;
};

inline void String2Lower(std::string& data) {
    for (auto& c : data) {
        c = ::tolower(c);
    }
}

inline void String2Upper(std::string& data) {
    for (auto& c : data) {
        c = ::toupper(c);
    }
}

inline void RemoveSubfix(std::string& data, const std::string& subfix) {
    size_t pos = data.rfind(subfix);
    if (pos == data.npos) {
        return;
    }
    if ((data.length() - pos) == subfix.length()) {
        data = data.substr(0, pos);
    }
}

std::string WStringToUtf8(const std::wstring& wstr);
std::wstring Utf8ToWstring(const std::string& u8str);

}
#endif //STRING_EXTEN_HPP
