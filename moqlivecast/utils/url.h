#ifndef URL_H
#define URL_H

#include <string>
#include <stdint.h>
#include <stddef.h>

namespace cpp_streamer
{

	bool ParseUrl(const std::string& url, bool& isHttps, std::string& host, uint16_t& port, std::string& subpath);
	bool GetSrcDirPathAndFilename(const std::string& src_path, std::string& src_dir, std::string& filename);
}
#endif