#include "httpflv_server.hpp"
#include "httpflv_writer.hpp"
#include "media_stream_manager.hpp"
#include "utils/meta_log.hpp"
#include "utils/uuid.hpp"

#include <string>
#include <unordered_map>

namespace cpp_streamer
{
    std::unordered_map<std::string, HttpFlvWriter*> s_httpflv_handle_map;

    static void SendHttpError(std::shared_ptr<HttpResponse> response_ptr,
                              int code, const std::string &status,
                              const std::string &body) {
        if (!response_ptr) return;
        response_ptr->SetStatusCode(code);
        response_ptr->SetStatus(status);
        response_ptr->AddHeader("Access-Control-Allow-Origin", "*");
        response_ptr->AddHeader("Content-Type", "text/plain");
        response_ptr->Write(body.c_str(), body.size(), false);
    }

    void HttpFlvHandle(const HttpRequest* request, std::shared_ptr<HttpResponse> response_ptr) {
        Logger* logger = response_ptr->GetLogger();

        auto pos = request->uri_.find(".flv");
        if (pos == std::string::npos) {
            LogErrorf(logger, "http flv request uri error:%s", request->uri_.c_str());
            SendHttpError(response_ptr, 404, "Not Found", "not found");
            return;
        }
        std::string key = request->uri_.substr(0, pos);
        if (!key.empty() && key[0] == '/') {
            key = key.substr(1);
        }
        if (key.empty() || key.find('/') == std::string::npos) {
            LogErrorf(logger, "http flv key invalid:%s", request->uri_.c_str());
            SendHttpError(response_ptr, 400, "Bad Request", "bad key");
            return;
        }

        std::string uuid = UUID::MakeUUID2();

        LogInfof(logger, "http flv request key:%s, uuid:%s", key.c_str(), uuid.c_str());

        std::string app, stream, params;
        auto slash = key.find('/');
        if (slash != std::string::npos) {
            app = key.substr(0, slash);
            stream = key.substr(slash + 1);
        }
        for (const auto &kv : request->params) {
            if (kv.first == "app" || kv.first == "stream") continue;
            if (!params.empty()) params += "&";
            params += kv.first;
            params += "=";
            params += kv.second;
        }
        MetaLog::Instance().Event("httpflv_pull", "opensession", app, stream, params);
        MetaLog::Instance().Event("httpflv_pull", "openstream", app, stream, params);

        HttpFlvWriter* writer_p = new HttpFlvWriter(key, uuid, response_ptr, logger);

        s_httpflv_handle_map.insert(std::make_pair(uuid, writer_p));

        MediaStreamManager::AddPlayer(writer_p);
        return;
    }

    HttpFlvServer::HttpFlvServer(uv_loop_t* loop, const std::string& ip, uint16_t port, Logger* logger):TimerInterface(100)
        , server_(loop, ip, port, logger)
		, logger_(logger)
    {
        Run();
        StartTimer();
        LogInfof(logger_, "http flv server is listen on %s:%d", ip.c_str(), port);
    }

    HttpFlvServer::HttpFlvServer(uv_loop_t* loop, const std::string& ip, uint16_t port,
                                 const std::string& key_file, const std::string& cert_file,
                                 Logger* logger):TimerInterface(100)
        , server_(loop, ip, port, key_file, cert_file, logger)
        , logger_(logger)
    {
        Run();
        StartTimer();
        LogInfof(logger_, "http flv server https listen on %s:%d", ip.c_str(), port);
    }

    HttpFlvServer::~HttpFlvServer()
    {
        StopTimer();
        auto iter = s_httpflv_handle_map.begin();
        while (iter != s_httpflv_handle_map.end()) {
            HttpFlvWriter* writer_p = iter->second;
            const std::string key = writer_p->GetKey();
            std::string app, stream;
            auto slash = key.find('/');
            if (slash != std::string::npos) {
                app = key.substr(0, slash);
                stream = key.substr(slash + 1);
            }
            MetaLog::Instance().Event("httpflv_pull", "closesession",
                                      app, stream, "");
            MediaStreamManager::RemovePlayer(writer_p);
            delete writer_p;
            iter = s_httpflv_handle_map.erase(iter);
        }
    }

    void HttpFlvServer::Run() {
		server_.AddGetHandle("/", HttpFlvHandle);
        return;
    }

    bool HttpFlvServer::OnTimer() {
        OnCheckAlive();
        return timer_running_;
    }

    void HttpFlvServer::OnCheckAlive() {
        auto iter = s_httpflv_handle_map.begin();

        while (iter != s_httpflv_handle_map.end()) {
            HttpFlvWriter* writer_p = iter->second;
            bool is_alive = writer_p->IsAlive();
            if (!is_alive) {
                const std::string &key = writer_p->GetKey();
                std::string app, stream;
                auto slash = key.find('/');
                if (slash != std::string::npos) {
                    app = key.substr(0, slash);
                    stream = key.substr(slash + 1);
                }
                MetaLog::Instance().Event("httpflv_pull", "closesession",
                                          app, stream, "");
                MediaStreamManager::RemovePlayer(writer_p);
                s_httpflv_handle_map.erase(iter++);
                delete writer_p;
                continue;
            }
            iter++;
        }
        return;
    }

}
