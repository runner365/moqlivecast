#include "co_http_server.hpp"
#include "utils/co_pub.hpp"
#include "utils/json.hpp"

#include <sstream>

using namespace cpp_streamer;
using json = nlohmann::json;

Logger logger("CoHttpServerDemo");

const std::string send_msg = "hello world...";

CoVoidTask HandleGetRequest(std::shared_ptr<CoHttpRequest> request, std::shared_ptr<CoHttpResponse> response_ptr) {
    LogInfof(&logger, "handle GET request: %s", request->Dump().c_str());

    response_ptr->SetStatusCode(200);
    response_ptr->SetStatus("OK");
    response_ptr->AddHeader("Content-Type", "application/json");
    LogInfof(&logger, "set response...");
    try
    {
        json response_json = json();
        response_json["message"] = send_msg;
        response_json["code"] = 200;

        std::string response_str = response_json.dump();

        LogInfof(&logger, "Sending response: %s", response_str.c_str());
        response_ptr->Write(response_str.c_str(), response_str.length());
    }
    catch (const std::exception& e)
    {
        LogInfof(&logger, "send respoonse exception:%s", e.what());
    }


    co_return;
}

CoVoidTask HandlePostRequest(std::shared_ptr<CoHttpRequest> request, std::shared_ptr<CoHttpResponse> response_ptr) {
    LogInfof(&logger, "Received POST request: %s", request->Dump().c_str());

    response_ptr->SetStatusCode(200);
    response_ptr->SetStatus("OK");
    response_ptr->AddHeader("Content-Type", "application/json");
    json response_json;
    response_json["message"] = send_msg;
    response_json["code"] = 200;

    std::string response_str = response_json.dump();
    response_ptr->Write(response_str.c_str(), response_str.length());

    co_return;
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <port>" << std::endl;
        return 1;
    }
    uint16_t port = static_cast<uint16_t>(std::stoi(argv[1]));

    uv_loop_t* loop = uv_default_loop();

    std::string host = "0.0.0.0";

    logger.SetFilename("co_http_server_demo.log");
    logger.SetLevel(LOGGER_INFO_LEVEL);

	LogInfof(&logger, "Starting CoHttpServer on %s:%d", host.c_str(), port);
    CoHttpServer http_server(loop, host, port, &logger);
    http_server.AddGetHandle("/api/get", HandleGetRequest);
    http_server.AddPostHandle("/api/post", HandlePostRequest);

    http_server.Run();

	LogInfof(&logger, "uv running...");
    uv_run(loop, UV_RUN_DEFAULT);

    return 0;
}
