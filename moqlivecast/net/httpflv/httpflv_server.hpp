#ifndef HTTP_FLV_SERVER_HPP
#define HTTP_FLV_SERVER_HPP
#ifdef _WIN64
#define WIN32_LEAN_AND_MEAN  // ���� Windows �ɰ�����ͷ�ļ������� winsock.h��
#endif
#include "net/http/http_server.hpp"
#include "timer.hpp"
#include "utils/logger.hpp"

namespace cpp_streamer
{ 
    class HttpFlvServer : public TimerInterface
    {
    public:
        HttpFlvServer(uv_loop_t* loop, const std::string& ip, uint16_t port, Logger* logger);
        HttpFlvServer(uv_loop_t* loop, const std::string& ip, uint16_t port,
                      const std::string& key_file, const std::string& cert_file,
                      Logger* logger);
        virtual ~HttpFlvServer();

    public:
        virtual bool OnTimer() override;

    private:
        void Run();
        void OnCheckAlive();

    private:
        HttpServer server_;
        Logger* logger_ = nullptr;
    };
}

#endif //HTTP_FLV_SERVER_HPP