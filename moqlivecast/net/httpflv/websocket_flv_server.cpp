#include "websocket_flv_server.hpp"

#include "media_stream_manager.hpp"
#include "net/http/websocket/websocket_session.hpp"
#include "utils/meta_log.hpp"
#include "utils/uuid.hpp"
#include "websocket_flv_writer.hpp"

#include <utility>

namespace cpp_streamer {

WebSocketFlvServer::WebSocketFlvServer(uv_loop_t* loop, const std::string& ip,
                                       uint16_t port,
                                       const std::string& key_file,
                                       const std::string& cert_file,
                                       Logger* logger)
    : TimerInterface(100)
    , logger_(logger) {
    if (key_file.empty() || cert_file.empty()) {
        server_.reset(new WebSocketServer(ip, port, loop, logger));
    } else {
        server_.reset(new WebSocketServer(ip, port, loop, key_file, cert_file,
                                          logger));
    }
    server_->AddHandle("/wsflv",
        [this](const std::string& uri, WebSocketSession* session) {
            OnWsSession(uri, session);
        });
    StartTimer();
    LogInfof(logger_, "websocket-flv listen on %s:%u path=/wsflv",
             ip.c_str(), (unsigned)port);
}

WebSocketFlvServer::~WebSocketFlvServer() {
    StopTimer();
    for (auto& item : writers_) {
        WebSocketFlvWriter* writer = item.second;
        std::string key = writer->GetKey();
        std::string app, stream;
        auto slash = key.find('/');
        if (slash != std::string::npos) {
            app = key.substr(0, slash);
            stream = key.substr(slash + 1);
        }
        MetaLog::Instance().Event("wsflv_pull", "closesession", app, stream, "");
        MediaStreamManager::RemovePlayer(writer);
        writer->CloseWriter();
        delete writer;
    }
    writers_.clear();
}

void WebSocketFlvServer::OnWsSession(const std::string& uri,
                                     WebSocketSession* session) {
    if (!session) return;
    const auto& query = session->GetQueryMap();
    auto app_it = query.find("app");
    auto stream_it = query.find("stream");
    if (app_it == query.end() || stream_it == query.end() ||
        app_it->second.empty() || stream_it->second.empty()) {
        LogErrorf(logger_, "ws-flv missing app/stream uri:%s", uri.c_str());
        session->CloseSession();
        return;
    }

    const std::string& app = app_it->second;
    const std::string& stream = stream_it->second;
    const std::string key = app + "/" + stream;
    const std::string uuid = UUID::MakeUUID2();

    MetaLog::Instance().Event("wsflv_pull", "opensession", app, stream, "");
    MetaLog::Instance().Event("wsflv_pull", "openstream", app, stream, "");
    LogInfof(logger_, "ws-flv request key:%s uuid:%s remote:%s",
             key.c_str(), uuid.c_str(), session->GetRemoteAddress().c_str());

    std::shared_ptr<WebSocketSession> shared;
    try {
        shared = session->shared_from_this();
    } catch (const std::bad_weak_ptr&) {
        LogErrorf(logger_, "ws-flv session has no shared owner");
        session->CloseSession();
        return;
    }

    auto* writer = new WebSocketFlvWriter(key, uuid, shared, logger_);
    session->SetSessionCallback(writer);
    writers_[uuid] = writer;
    pending_players_.push_back(writer);
}

void WebSocketFlvServer::OnCheckAlive() {
    auto iter = writers_.begin();
    while (iter != writers_.end()) {
        WebSocketFlvWriter* writer = iter->second;
        if (writer->IsAlive()) {
            ++iter;
            continue;
        }

        std::string key = writer->GetKey();
        std::string app, stream;
        auto slash = key.find('/');
        if (slash != std::string::npos) {
            app = key.substr(0, slash);
            stream = key.substr(slash + 1);
        }
        MetaLog::Instance().Event("wsflv_pull", "closesession", app, stream, "");
        MediaStreamManager::RemovePlayer(writer);
        writer->CloseWriter();
        delete writer;
        iter = writers_.erase(iter);
    }
}

bool WebSocketFlvServer::OnTimer() {
    for (WebSocketFlvWriter* writer : pending_players_) {
        if (writers_.find(writer->GetWriterId()) != writers_.end()) {
            MediaStreamManager::AddPlayer(writer);
        }
    }
    pending_players_.clear();
    OnCheckAlive();
    return timer_running_;
}

} /* namespace cpp_streamer */
