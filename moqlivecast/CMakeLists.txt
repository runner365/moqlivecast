# Media over WebTransport — C++ 服务端
# 可执行文件：moqlivecast

# ── 底层 C 协议栈 ────────────────────────────────
set(MOQ_SERVER_STACK
    ${CMAKE_SOURCE_DIR}/http3/webtransport_server_api.c
    ${CMAKE_SOURCE_DIR}/http3/http3_frame.c
    ${CMAKE_SOURCE_DIR}/http3/http3_server.c
    ${CMAKE_SOURCE_DIR}/http3/http3_server_api.c
    ${CMAKE_SOURCE_DIR}/http3/qpack.c
    ${CMAKE_SOURCE_DIR}/http3/huffman.c
    ${CMAKE_SOURCE_DIR}/http3/webtransport.c
    ${CMAKE_SOURCE_DIR}/udp/udp_client.c
    ${CMAKE_SOURCE_DIR}/udp/udp_server.c
    ${CMAKE_SOURCE_DIR}/tls/tls_common.c
    ${CMAKE_SOURCE_DIR}/tls/tls_client.c
    ${CMAKE_SOURCE_DIR}/tls/tls_server.c
    ${CMAKE_SOURCE_DIR}/quic/quic_packet.c
    ${CMAKE_SOURCE_DIR}/quic/quic_crypto.c
    ${CMAKE_SOURCE_DIR}/quic/quic_connection.c
    ${CMAKE_SOURCE_DIR}/quic/quic_listener.c
    ${CMAKE_SOURCE_DIR}/quic/quic_stream.c
    ${CMAKE_SOURCE_DIR}/quic/quic_recovery.c
    ${CMAKE_SOURCE_DIR}/quic/quic_timer.c
    ${CMAKE_SOURCE_DIR}/quic/quic_client_api.c
    ${CMAKE_SOURCE_DIR}/quic/congestion_control/quic_cc_newreno.c
    ${CMAKE_SOURCE_DIR}/quic/congestion_control/quic_cc_bbr.c
    ${CMAKE_SOURCE_DIR}/quic/congestion_control/quic_cc_cubic.c
    ${CMAKE_SOURCE_DIR}/logger/logger.c
)

# ── 本目录 C++ 源 / 头文件 ───────────────────────
file(GLOB_RECURSE MOQ_CXX_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
)
file(GLOB_RECURSE MOQ_CXX_HEADERS CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.h"
)

# 独立 demo / echo 带自己的 main，不编进 moqlivecast
list(FILTER MOQ_CXX_SOURCES EXCLUDE REGEX ".*/[^/]*(_demo|_echo)\\.cpp$")
list(FILTER MOQ_CXX_SOURCES EXCLUDE REGEX ".*/net/http/co_http/.*")
list(FILTER MOQ_CXX_SOURCES EXCLUDE REGEX ".*/net/tcp/co_tcp/.*")
list(FILTER MOQ_CXX_HEADERS EXCLUDE REGEX ".*/net/http/co_http/.*")
list(FILTER MOQ_CXX_HEADERS EXCLUDE REGEX ".*/net/tcp/co_tcp/.*")

add_executable(moqlivecast
    ${MOQ_CXX_SOURCES}
    ${MOQ_CXX_HEADERS}
    ${MOQ_SERVER_STACK}
)

set_target_properties(moqlivecast PROPERTIES
    CXX_STANDARD 14
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    # 输出到 build/bin/：不能放 build/ 根目录 —— target 名与本子项目的构建目录
    # 同名（build/moqlivecast/ 是目录），会 "Is a directory" 链接失败。
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

target_link_libraries(moqlivecast PRIVATE
    uv_a
    ssl
    crypto
    yaml-cpp
)

# macOS：iconv 在 libiconv；Linux 一般已在 libc 中
if(APPLE)
    target_link_libraries(moqlivecast PRIVATE iconv)
endif()

target_include_directories(moqlivecast PRIVATE
    ${CMAKE_SOURCE_DIR}/3rdparty/libuv/include
    ${OPENSSL_INSTALL_DIR}/include
    ${CMAKE_SOURCE_DIR}/udp
    ${CMAKE_SOURCE_DIR}/tls
    ${CMAKE_SOURCE_DIR}/quic
    ${CMAKE_SOURCE_DIR}/quic/congestion_control
    ${CMAKE_SOURCE_DIR}/http3
    ${CMAKE_SOURCE_DIR}/logger
    ${CMAKE_SOURCE_DIR}/3rdparty/yaml-cpp/include
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/config
    ${CMAKE_CURRENT_SOURCE_DIR}/format
    ${CMAKE_CURRENT_SOURCE_DIR}/format/amf
    ${CMAKE_CURRENT_SOURCE_DIR}/format/flv
    ${CMAKE_CURRENT_SOURCE_DIR}/format/mp4
    ${CMAKE_CURRENT_SOURCE_DIR}/format/rtc_sdp
    ${CMAKE_CURRENT_SOURCE_DIR}/net
    ${CMAKE_CURRENT_SOURCE_DIR}/net/http
    ${CMAKE_CURRENT_SOURCE_DIR}/net/http/websocket
    ${CMAKE_CURRENT_SOURCE_DIR}/net/httpflv
    ${CMAKE_CURRENT_SOURCE_DIR}/net/rtmp
    ${CMAKE_CURRENT_SOURCE_DIR}/net/rtprtcp
    ${CMAKE_CURRENT_SOURCE_DIR}/net/stun
    ${CMAKE_CURRENT_SOURCE_DIR}/net/tcp
    ${CMAKE_CURRENT_SOURCE_DIR}/net/udp
    ${CMAKE_CURRENT_SOURCE_DIR}/utils
    ${CMAKE_CURRENT_SOURCE_DIR}/utils/av
    ${CMAKE_CURRENT_SOURCE_DIR}/wt_server
)

add_dependencies(moqlivecast ssl crypto)
