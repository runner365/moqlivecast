#ifndef BASE64_HPP
#define BASE64_HPP

#include <string>

namespace cpp_streamer
{
std::string Base64Encode(const unsigned char* , unsigned int len);
std::string Base64Decode(std::string const& s);
}
#endif