#include "stringex.hpp"

#ifdef _WIN32
#include <Windows.h>
#else
#include <iconv.h>
#include <cstring>
#endif

namespace cpp_streamer
{
    std::string WStringToUtf8(const std::wstring& wstr) {
        if (wstr.empty()) {
            return "";
        }

#ifdef _WIN32
        // Windows implementation using Win32 API
        int buffer_size = WideCharToMultiByte(
            CP_UTF8,
            0,
            wstr.c_str(),
            static_cast<int>(wstr.size()),
            nullptr,
            0,
            nullptr,
            nullptr
        );

        if (buffer_size == 0) {
            throw std::runtime_error("WideCharToMultiByte failed to calculate buffer size");
        }

        std::string result(buffer_size, 0);
        int conversion_result = WideCharToMultiByte(
            CP_UTF8,
            0,
            wstr.c_str(),
            static_cast<int>(wstr.size()),
            &result[0],
            buffer_size,
            nullptr,
            nullptr
        );

        if (conversion_result == 0) {
            throw std::runtime_error("WideCharToMultiByte conversion failed");
        }

        return result;
#else
        // Linux/macOS implementation using iconv
        iconv_t cd = iconv_open("UTF-8", "WCHAR_T");
        if (cd == (iconv_t)-1) {
            throw std::runtime_error("Failed to initialize iconv converter");
        }

        const wchar_t* input_ptr = wstr.c_str();
        size_t input_size = wstr.size() * sizeof(wchar_t);

        // Allocate buffer with generous size (UTF-8 needs max 4 bytes per character)
        size_t output_size = wstr.size() * 4 + 1;
        char* output_buf = new char[output_size];
        char* output_ptr = output_buf;

        // iconv expects mutable char**; copy to a local mutable pointer to avoid casting away constness
        char* inbuf = reinterpret_cast<char*>(const_cast<wchar_t*>(input_ptr));
        size_t inbytes_left = input_size;
        size_t outbytes_left = output_size;

        if (iconv(cd, &inbuf, &inbytes_left, &output_ptr, &outbytes_left) == (size_t)-1) {
            iconv_close(cd);
            delete[] output_buf;
            throw std::runtime_error("iconv conversion failed");
        }

        *output_ptr = '\0';
        std::string result(output_buf);

        iconv_close(cd);
        delete[] output_buf;

        return result;
#endif
    }

    std::wstring Utf8ToWstring(const std::string& u8str) {
        if (u8str.empty()) {
            return L"";
        }

#ifdef _WIN32
        // Windows implementation using Win32 API
        int buffer_size = MultiByteToWideChar(
            CP_UTF8,
            MB_ERR_INVALID_CHARS,
            u8str.c_str(),
            static_cast<int>(u8str.size()),
            nullptr,
            0
        );

        if (buffer_size == 0) {
            throw std::runtime_error("MultiByteToWideChar failed to calculate buffer size");
        }

        std::wstring result(buffer_size, 0);
        int conversion_result = MultiByteToWideChar(
            CP_UTF8,
            MB_ERR_INVALID_CHARS,
            u8str.c_str(),
            static_cast<int>(u8str.size()),
            &result[0],
            buffer_size
        );

        if (conversion_result == 0) {
            throw std::runtime_error("MultiByteToWideChar conversion failed");
        }

        return result;
#else
        // Linux/macOS implementation using iconv
        iconv_t cd = iconv_open("WCHAR_T", "UTF-8");
        if (cd == (iconv_t)-1) {
            throw std::runtime_error("Failed to initialize iconv converter");
        }

        char* input_ptr = const_cast<char*>(u8str.c_str());
        size_t input_size = u8str.size();

        // Allocate buffer for wide characters
        size_t output_size = u8str.size() * sizeof(wchar_t) + sizeof(wchar_t);
        wchar_t* output_buf = new wchar_t[output_size / sizeof(wchar_t)];
        char* output_ptr = reinterpret_cast<char*>(output_buf);
        size_t output_buf_size = output_size;

        if (iconv(cd, &input_ptr, &input_size, &output_ptr, &output_buf_size) == (size_t)-1) {
            iconv_close(cd);
            delete[] output_buf;
            throw std::runtime_error("iconv conversion failed");
        }

        output_buf[(output_size - output_buf_size) / sizeof(wchar_t)] = L'\0';
        std::wstring result(output_buf);

        iconv_close(cd);
        delete[] output_buf;

        return result;
#endif
    }

}
