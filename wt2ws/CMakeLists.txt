# ============================================
# wt2ws_proxy — WebTransport <-> WebSocket relay
#
#   browser --WebTransport--> wt2ws_proxy --WebSocket--> backend
#
# The WT leg is reliable (QUIC stream) and framed by wt2ws_frame; the WS leg is
# the traditional WebSocket inside the datacenter.  The proxy is payload
# agnostic: it only preserves message boundaries and the text/binary bit.
# ============================================

set(WT2WS_DIR ${CMAKE_CURRENT_SOURCE_DIR})
set(WT2WS_ROOT ${CMAKE_SOURCE_DIR})

# The shared source lists are defined relative to the repository root; inside a
# subdirectory every entry must be made absolute or CMake looks under wt2ws/.
function(wt2ws_abs_sources out)
    set(_tmp "")
    foreach(_s ${ARGN})
        list(APPEND _tmp "${WT2WS_ROOT}/${_s}")
    endforeach()
    set(${out} ${_tmp} PARENT_SCOPE)
endfunction()

wt2ws_abs_sources(WT2WS_H3_SOURCES ${H3_COMMON_SOURCES})
wt2ws_abs_sources(WT2WS_UDP_SOURCES ${UDP_COMMON_SOURCES})
wt2ws_abs_sources(WT2WS_TLS_SOURCES ${TLS_COMMON_SOURCES})
wt2ws_abs_sources(WT2WS_QUIC_SOURCES ${QUIC_COMMON_SOURCES})
wt2ws_abs_sources(WT2WS_LOGGER_SOURCES ${LOGGER_SOURCES})

# The repository's WebSocket client (moqlivecast/net/http/websocket) is reused
# as-is.  It is currently unreferenced by moqlivecast, so keep it in its own
# small static library and validate it against py_ws_echo before relying on it.
set(WT2WS_WS_SOURCES
    ${CMAKE_SOURCE_DIR}/moqlivecast/net/http/websocket/websocket_client.cpp
    ${CMAKE_SOURCE_DIR}/moqlivecast/net/http/websocket/websocket_frame.cpp
    ${CMAKE_SOURCE_DIR}/moqlivecast/net/http/websocket/ws_session_base.cpp
    ${CMAKE_SOURCE_DIR}/moqlivecast/net/http/websocket/websocket_pub.cpp
    ${CMAKE_SOURCE_DIR}/moqlivecast/net/http/http_client.cpp
    ${CMAKE_SOURCE_DIR}/moqlivecast/utils/base64.cpp
    ${CMAKE_SOURCE_DIR}/moqlivecast/utils/byte_crypto.cpp
    ${CMAKE_SOURCE_DIR}/moqlivecast/utils/stringex.cpp
    ${CMAKE_SOURCE_DIR}/moqlivecast/utils/timeex.cpp
    ${CMAKE_SOURCE_DIR}/moqlivecast/utils/timer.cpp
)

add_library(wt2ws_wsclient STATIC ${WT2WS_WS_SOURCES})
set_target_properties(wt2ws_wsclient PROPERTIES
    CXX_STANDARD 14
    CXX_STANDARD_REQUIRED ON
)
target_include_directories(wt2ws_wsclient PUBLIC
    ${WT2WS_ROOT}/3rdparty/libuv/include
    ${OPENSSL_INSTALL_DIR}/include
    ${WT2WS_ROOT}/udp ${WT2WS_ROOT}/tls ${WT2WS_ROOT}/quic
    ${WT2WS_ROOT}/quic/congestion_control ${WT2WS_ROOT}/http3
    ${WT2WS_ROOT}/logger
    ${CMAKE_SOURCE_DIR}/moqlivecast
    ${CMAKE_SOURCE_DIR}/moqlivecast/utils
    ${CMAKE_SOURCE_DIR}/moqlivecast/net/http
    ${CMAKE_SOURCE_DIR}/moqlivecast/net/http/websocket
    ${CMAKE_SOURCE_DIR}/moqlivecast/net/tcp
)
# Public: the proxy target inherits uv/ssl/crypto and the include dirs above.
target_link_libraries(wt2ws_wsclient PUBLIC uv_a ssl crypto)
add_dependencies(wt2ws_wsclient ssl crypto)

set(WT2WS_SOURCES
    wt2ws_proxy.cpp
    wt2ws_config.cpp
    wt2ws_frame.cpp
    wt2ws_backend.cpp
    wt2ws_session.cpp
    wt2ws_server.cpp
)

add_executable(wt2ws_proxy
    ${WT2WS_SOURCES}
    ${WT2WS_ROOT}/http3/webtransport_server_api.c
    ${WT2WS_H3_SOURCES}
    ${WT2WS_UDP_SOURCES} ${WT2WS_TLS_SOURCES}
    ${WT2WS_QUIC_SOURCES} ${WT2WS_LOGGER_SOURCES}
)
set_target_properties(wt2ws_proxy PROPERTIES
    CXX_STANDARD 14
    CXX_STANDARD_REQUIRED ON
)
target_link_libraries(wt2ws_proxy PRIVATE wt2ws_wsclient uv_a ssl crypto)
target_include_directories(wt2ws_proxy PRIVATE
    ${WT2WS_DIR}
    ${WT2WS_ROOT}/3rdparty/libuv/include
    ${OPENSSL_INSTALL_DIR}/include
    ${WT2WS_ROOT}/udp ${WT2WS_ROOT}/tls ${WT2WS_ROOT}/quic
    ${WT2WS_ROOT}/quic/congestion_control ${WT2WS_ROOT}/http3
    ${WT2WS_ROOT}/logger
    ${CMAKE_SOURCE_DIR}/moqlivecast
    ${CMAKE_SOURCE_DIR}/moqlivecast/utils
    ${CMAKE_SOURCE_DIR}/moqlivecast/net/http
    ${CMAKE_SOURCE_DIR}/moqlivecast/net/http/websocket
    ${CMAKE_SOURCE_DIR}/moqlivecast/net/tcp
)
add_dependencies(wt2ws_proxy ssl crypto)

# ============================================
# Smoke test: WT stream -> proxy -> py_ws_echo -> back
# ============================================
add_executable(wt2ws_smoke
    tests/wt2ws_smoke.c
    ${WT2WS_ROOT}/http3/webtransport_client_api.c
    ${WT2WS_H3_SOURCES}
    ${WT2WS_UDP_SOURCES} ${WT2WS_TLS_SOURCES}
    ${WT2WS_QUIC_SOURCES} ${WT2WS_LOGGER_SOURCES}
)
target_link_libraries(wt2ws_smoke PRIVATE uv_a ssl crypto)
target_include_directories(wt2ws_smoke PRIVATE
    ${WT2WS_ROOT}/3rdparty/libuv/include
    ${OPENSSL_INSTALL_DIR}/include
    ${WT2WS_ROOT}/udp ${WT2WS_ROOT}/tls ${WT2WS_ROOT}/quic
    ${WT2WS_ROOT}/quic/congestion_control ${WT2WS_ROOT}/http3
    ${WT2WS_ROOT}/logger
)
add_dependencies(wt2ws_smoke ssl crypto)
