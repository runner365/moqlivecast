#include "url.h"

namespace cpp_streamer
{

	bool ParseUrl(const std::string& url, bool& isHttps, std::string& host, uint16_t& port, std::string& subpath) {
		isHttps = false;
		host.clear();
		port = 80;
		subpath = "/";

		size_t pos = url.find("://");
		if (pos == std::string::npos) {
			return false;
		}
		std::string schema = url.substr(0, pos);
		if (schema == "http") {
			isHttps = false;
			port = 80;
		} else if (schema == "https") {
			isHttps = true;
			port = 443;
		} else {
			return false;
		}
		size_t host_start = pos + 3;
		size_t path_start = url.find("/", host_start);
		if (path_start == std::string::npos) {
			host = url.substr(host_start);
			subpath = "/";
			return true;
		} else {
			host = url.substr(host_start, path_start - host_start);
			subpath = url.substr(path_start);
		}
		pos = host.find(":");
		if (pos != std::string::npos) {
			std::string port_str = host.substr(pos + 1);
			port = (uint16_t)atoi(port_str.c_str());
			host = host.substr(0, pos);
		}
		return true;
	}
	bool GetSrcDirPathAndFilename(const std::string& src_path, std::string& src_dir, std::string& filename) {
		if (src_path.empty()) {
			return false;
		}
#if defined(_WIN32) || defined(_WIN64)
		size_t pos = src_path.find_last_of("\\/");
		if (pos == std::string::npos || pos <= 4) {
			return false;
		}
		else {
			src_dir = src_path.substr(0, pos);
			filename = src_path.substr(pos + 1);
		}
#else
		size_t pos = src_path.find_last_of("/\\");
		if (pos == std::string::npos) {
			src_dir = ".";
			filename = src_path;
		}
		else if (pos == 0) {
			src_dir = src_path.substr(0, 1);
			filename = src_path.substr(1);
		}
		else {
			src_dir = src_path.substr(0, pos);
			filename = src_path.substr(pos + 1);
		}
#endif
		return true;
	}
}