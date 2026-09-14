#ifndef MOQ_HANDLER_HPP
#define MOQ_HANDLER_HPP

#include "wt_server/wt_server.hpp"

#include <memory>
#include <string>
#include <unordered_map>

namespace cpp_streamer {

class WtFlvWriter;

class MoqHandler {
public:
    static void OnSession(WTServerSession &sess, const std::string &path);
    static void OnStreamData(WTServerSession &sess, WTServerStream &st,
                             const uint8_t *data, size_t len,
                             const std::string &path);
    static void OnSessionClose(WTServerSession &sess, const std::string &path);

    static void FeedFlv(const std::string &app, const std::string &stream,
                        const uint8_t *data, size_t len);
    static void DropPublisher(const std::string &app, const std::string &stream);

private:
    static void OnPullOpen(WTServerSession &sess, WTServerStream &st);

    static std::unordered_map<std::string, std::unique_ptr<WtFlvWriter>> pull_writers_;
};

} /* namespace cpp_streamer */

#endif /* MOQ_HANDLER_HPP */
