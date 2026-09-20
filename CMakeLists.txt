cmake_minimum_required(VERSION 3.10)
project(moq LANGUAGES C CXX)

# 禁用 libuv 的测试和基准测试编译
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)

add_subdirectory(3rdparty/libuv)

# ============================================
# yaml-cpp（Media over WT 配置解析）
# ============================================
set(YAML_CPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(YAML_CPP_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(YAML_CPP_INSTALL OFF CACHE BOOL "" FORCE)
set(YAML_CPP_FORMAT_SOURCE OFF CACHE BOOL "" FORCE)
add_subdirectory(3rdparty/yaml-cpp)

# ============================================
# OpenSSL 构建 (ExternalProject)
# ============================================
include(ExternalProject)

# 检测 OpenSSL Configure target
if(APPLE)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
        set(OPENSSL_TARGET "darwin64-arm64-cc")
    else()
        set(OPENSSL_TARGET "darwin64-x86_64-cc")
    endif()
elseif(UNIX)
    # Linux：按架构选择，避免写死 x86_64（ARM64 Debian 会配置失败）
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64|ARM64")
        set(OPENSSL_TARGET "linux-aarch64")
    elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64|AMD64")
        set(OPENSSL_TARGET "linux-x86_64")
    else()
        # 未识别架构 → 不指定 target，让 OpenSSL Configure 自动探测
        set(OPENSSL_TARGET "")
    endif()
endif()

set(OPENSSL_SRC_DIR    "${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/openssl-3.5.8")
set(OPENSSL_INSTALL_DIR "${CMAKE_BINARY_DIR}/openssl_install")

# 提前创建目录，避免 IMPORTED target 因路径不存在而报错
file(MAKE_DIRECTORY ${OPENSSL_INSTALL_DIR}/include)
file(MAKE_DIRECTORY ${OPENSSL_INSTALL_DIR}/lib)

# 并行度：在 configure 时用 CMake 的 ProcessorCount 算好，避免 BUILD_COMMAND 里嵌套 $(...)
include(ProcessorCount)
ProcessorCount(NPROC)
if(NPROC EQUAL 0)
    set(NPROC 4)
endif()

ExternalProject_Add(openssl_ext
    SOURCE_DIR        ${OPENSSL_SRC_DIR}
    CONFIGURE_COMMAND ${OPENSSL_SRC_DIR}/Configure
                      ${OPENSSL_TARGET}
                      --prefix=${OPENSSL_INSTALL_DIR}
                      --openssldir=${OPENSSL_INSTALL_DIR}
                      --libdir=lib
                      no-shared
    BUILD_COMMAND     make -j${NPROC}
    INSTALL_COMMAND   make install_sw
    BUILD_BYPRODUCTS  ${OPENSSL_INSTALL_DIR}/lib/libssl.a
                      ${OPENSSL_INSTALL_DIR}/lib/libcrypto.a
)

# 创建 IMPORTED 库供各目标链接
add_library(ssl    STATIC IMPORTED GLOBAL)
add_library(crypto STATIC IMPORTED GLOBAL)

set_target_properties(ssl PROPERTIES
    IMPORTED_LOCATION ${OPENSSL_INSTALL_DIR}/lib/libssl.a
)
set_target_properties(crypto PROPERTIES
    IMPORTED_LOCATION ${OPENSSL_INSTALL_DIR}/lib/libcrypto.a
)

add_dependencies(ssl    openssl_ext)
add_dependencies(crypto openssl_ext)

# ============================================
# 公共 include 和源文件
# ============================================
set(UDP_COMMON_SOURCES
    udp/udp_client.c
    udp/udp_server.c
)

set(LOGGER_SOURCES logger/logger.c)

# ============================================
# UDP echo（源码保留，开源默认不编译）
# ============================================
if(FALSE)
add_executable(udp_echo_client udp/udp_echo_client.c ${UDP_COMMON_SOURCES} ${LOGGER_SOURCES})
target_link_libraries(udp_echo_client PRIVATE uv_a)
target_include_directories(udp_echo_client PRIVATE
    3rdparty/libuv/include
    udp
    logger
)

add_executable(udp_echo_server udp/udp_echo_server.c ${UDP_COMMON_SOURCES} ${LOGGER_SOURCES})
target_link_libraries(udp_echo_server PRIVATE uv_a)
target_include_directories(udp_echo_server PRIVATE
    3rdparty/libuv/include
    udp
    logger
)
endif()

# ============================================
# TLS 公共源文件
# ============================================
set(TLS_COMMON_SOURCES
    tls/tls_common.c
    tls/tls_client.c
    tls/tls_server.c
)

# ============================================
# TLS echo（源码保留，开源默认不编译）
# ============================================
if(FALSE)
add_executable(tls_echo_client tls/tls_echo_client.c
    ${UDP_COMMON_SOURCES} ${TLS_COMMON_SOURCES} ${LOGGER_SOURCES})
target_link_libraries(tls_echo_client PRIVATE uv_a ssl crypto)
target_include_directories(tls_echo_client PRIVATE
    3rdparty/libuv/include
    ${OPENSSL_INSTALL_DIR}/include
    udp
    tls
    logger
)
add_dependencies(tls_echo_client ssl crypto)

add_executable(tls_echo_server tls/tls_echo_server.c
    ${UDP_COMMON_SOURCES} ${TLS_COMMON_SOURCES} ${LOGGER_SOURCES})
target_link_libraries(tls_echo_server PRIVATE uv_a ssl crypto)
target_include_directories(tls_echo_server PRIVATE
    3rdparty/libuv/include
    ${OPENSSL_INSTALL_DIR}/include
    udp
    tls
    logger
)
add_dependencies(tls_echo_server ssl crypto)
endif()

# ============================================
# QUIC 公共源文件
# ============================================
set(QUIC_COMMON_SOURCES
    quic/quic_packet.c
    quic/quic_crypto.c
    quic/quic_connection.c
    quic/quic_listener.c
    quic/quic_stream.c
    quic/quic_recovery.c
    quic/quic_timer.c
    quic/pacing_send.c
    quic/quic_client_api.c
    quic/congestion_control/quic_cc_newreno.c
    quic/congestion_control/quic_cc_bbr.c
    quic/congestion_control/quic_cc_cubic.c
)

# ============================================
# QUIC / datagram echo（源码保留，开源默认不编译）
# ============================================
if(FALSE)
add_executable(quic_echo_client
    quic/quic_echo_client.c
    ${UDP_COMMON_SOURCES} ${TLS_COMMON_SOURCES}
    ${QUIC_COMMON_SOURCES} ${LOGGER_SOURCES})
target_link_libraries(quic_echo_client PRIVATE uv_a ssl crypto)
target_include_directories(quic_echo_client PRIVATE
    3rdparty/libuv/include
    ${OPENSSL_INSTALL_DIR}/include
    udp
    tls
    quic
    quic/congestion_control
    logger
)
add_dependencies(quic_echo_client ssl crypto)

add_executable(quic_echo_server
    quic/quic_echo_server.c
    ${UDP_COMMON_SOURCES} ${TLS_COMMON_SOURCES}
    ${QUIC_COMMON_SOURCES} ${LOGGER_SOURCES})
target_link_libraries(quic_echo_server PRIVATE uv_a ssl crypto)
target_include_directories(quic_echo_server PRIVATE
    3rdparty/libuv/include
    ${OPENSSL_INSTALL_DIR}/include
    udp
    tls
    quic
    quic/congestion_control
    logger
)
add_dependencies(quic_echo_server ssl crypto)

add_executable(quic_datagram_echo_client
    quic/quic_datagram_echo_client.c
    ${UDP_COMMON_SOURCES} ${TLS_COMMON_SOURCES}
    ${QUIC_COMMON_SOURCES} ${LOGGER_SOURCES})
target_link_libraries(quic_datagram_echo_client PRIVATE uv_a ssl crypto)
target_include_directories(quic_datagram_echo_client PRIVATE
    3rdparty/libuv/include
    ${OPENSSL_INSTALL_DIR}/include
    udp
    tls
    quic
    quic/congestion_control
    logger
)
add_dependencies(quic_datagram_echo_client ssl crypto)

add_executable(quic_datagram_echo_server
    quic/quic_datagram_echo_server.c
    ${UDP_COMMON_SOURCES} ${TLS_COMMON_SOURCES}
    ${QUIC_COMMON_SOURCES} ${LOGGER_SOURCES})
target_link_libraries(quic_datagram_echo_server PRIVATE uv_a ssl crypto)
target_include_directories(quic_datagram_echo_server PRIVATE
    3rdparty/libuv/include
    ${OPENSSL_INSTALL_DIR}/include
    udp
    tls
    quic
    quic/congestion_control
    logger
)
add_dependencies(quic_datagram_echo_server ssl crypto)
endif()

# ============================================
# HTTP/3 公共源文件
# ============================================
set(H3_COMMON_SOURCES
    http3/http3_frame.c
    http3/http3_server.c
    http3/http3_server_api.c
    http3/qpack.c
    http3/huffman.c
    http3/webtransport.c
)

# ============================================
# HTTP/3 / 早期 WT echo（源码保留，开源默认不编译）
# ============================================
if(FALSE)
add_executable(http3_echo_server
    http3/http3_echo_server.c
    ${H3_COMMON_SOURCES}
    ${UDP_COMMON_SOURCES} ${TLS_COMMON_SOURCES}
    ${QUIC_COMMON_SOURCES} ${LOGGER_SOURCES})
target_link_libraries(http3_echo_server PRIVATE uv_a ssl crypto)
target_include_directories(http3_echo_server PRIVATE
    3rdparty/libuv/include
    ${OPENSSL_INSTALL_DIR}/include
    udp
    tls
    quic
    quic/congestion_control
    http3
    logger
)
add_dependencies(http3_echo_server ssl crypto)

add_executable(http3_echo_client
    http3/http3_echo_client.c
    ${H3_COMMON_SOURCES}
    ${UDP_COMMON_SOURCES} ${TLS_COMMON_SOURCES}
    ${QUIC_COMMON_SOURCES} ${LOGGER_SOURCES})
target_link_libraries(http3_echo_client PRIVATE uv_a ssl crypto)
target_include_directories(http3_echo_client PRIVATE
    3rdparty/libuv/include
    ${OPENSSL_INSTALL_DIR}/include
    udp
    tls
    quic
    quic/congestion_control
    http3
    logger
)
add_dependencies(http3_echo_client ssl crypto)

add_executable(http3_wt_echo_client
    http3/http3_wt_echo_client.c
    ${H3_COMMON_SOURCES}
    ${UDP_COMMON_SOURCES} ${TLS_COMMON_SOURCES}
    ${QUIC_COMMON_SOURCES} ${LOGGER_SOURCES})
target_link_libraries(http3_wt_echo_client PRIVATE uv_a ssl crypto)
target_include_directories(http3_wt_echo_client PRIVATE
    3rdparty/libuv/include
    ${OPENSSL_INSTALL_DIR}/include
    udp
    tls
    quic
    quic/congestion_control
    http3
    logger
)
add_dependencies(http3_wt_echo_client ssl crypto)
endif()

# ============================================
# WebTransport 客户端静态库（供外部工具复用）
# 只含客户端路径：不含 http3_server / webtransport_session（服务端）
# ============================================
add_library(webtransport_client STATIC
    http3/webtransport_client_api.c
    http3/http3_frame.c
    http3/qpack.c
    http3/huffman.c
    ${UDP_COMMON_SOURCES} ${TLS_COMMON_SOURCES}
    ${QUIC_COMMON_SOURCES} ${LOGGER_SOURCES}
)

set_target_properties(webtransport_client PROPERTIES
    POSITION_INDEPENDENT_CODE ON
)

target_include_directories(webtransport_client PUBLIC
    3rdparty/libuv/include
    ${OPENSSL_INSTALL_DIR}/include
    udp tls quic quic/congestion_control http3 logger
)

# PUBLIC → 链接方自动继承 uv_a/ssl/crypto 和上面的 include
target_link_libraries(webtransport_client PUBLIC uv_a ssl crypto)
add_dependencies(webtransport_client ssl crypto)

# ============================================
# WebTransport Client API echo 测试
# ============================================
add_executable(wt_client_echo
    http3/wt_client_echo.c
    http3/webtransport_client_api.c
    ${H3_COMMON_SOURCES}
    ${UDP_COMMON_SOURCES} ${TLS_COMMON_SOURCES}
    ${QUIC_COMMON_SOURCES} ${LOGGER_SOURCES})
target_link_libraries(wt_client_echo PRIVATE uv_a ssl crypto)
target_include_directories(wt_client_echo PRIVATE
    3rdparty/libuv/include
    ${OPENSSL_INSTALL_DIR}/include
    udp
    tls
    quic
    quic/congestion_control
    http3
    logger
)
add_dependencies(wt_client_echo ssl crypto)

# ============================================
# WebTransport Server API echo 测试
# ============================================
add_executable(wt_echo_server
    http3/webtransport_echo_server.c
    http3/webtransport_server_api.c
    ${H3_COMMON_SOURCES}
    ${UDP_COMMON_SOURCES} ${TLS_COMMON_SOURCES}
    ${QUIC_COMMON_SOURCES} ${LOGGER_SOURCES})
target_link_libraries(wt_echo_server PRIVATE uv_a ssl crypto)
target_include_directories(wt_echo_server PRIVATE
    3rdparty/libuv/include
    ${OPENSSL_INSTALL_DIR}/include
    udp
    tls
    quic
    quic/congestion_control
    http3
    logger
)
add_dependencies(wt_echo_server ssl crypto)

# ============================================
# WebTransport 压力测试（大负载 echo）
# ============================================
add_executable(wt_stress_test
    http3/wt_stress_test.c
    http3/webtransport_client_api.c
    ${H3_COMMON_SOURCES}
    ${UDP_COMMON_SOURCES} ${TLS_COMMON_SOURCES}
    ${QUIC_COMMON_SOURCES} ${LOGGER_SOURCES})
target_link_libraries(wt_stress_test PRIVATE uv_a ssl crypto)
target_include_directories(wt_stress_test PRIVATE
    3rdparty/libuv/include
    ${OPENSSL_INSTALL_DIR}/include
    udp
    tls
    quic
    quic/congestion_control
    http3
    logger
)
add_dependencies(wt_stress_test ssl crypto)

# ============================================
# WebTransport Chat Server（信令扩展 demo，配合 http3/wt_chat_js_client）
# ============================================
add_executable(wt_chat_server
    http3/wt_chat_server.c
    http3/webtransport_server_api.c
    ${H3_COMMON_SOURCES}
    ${UDP_COMMON_SOURCES} ${TLS_COMMON_SOURCES}
    ${QUIC_COMMON_SOURCES} ${LOGGER_SOURCES})
target_link_libraries(wt_chat_server PRIVATE uv_a ssl crypto)
target_include_directories(wt_chat_server PRIVATE
    3rdparty/libuv/include
    ${OPENSSL_INSTALL_DIR}/include
    udp
    tls
    quic
    quic/congestion_control
    http3
    logger
)
add_dependencies(wt_chat_server ssl crypto)

# ============================================
# UDP Jitter Proxy（弱网模拟工具）
# ============================================
add_executable(udp_jitter_proxy tools/udp_jitter_proxy.c)
target_link_libraries(udp_jitter_proxy PRIVATE uv_a)
target_include_directories(udp_jitter_proxy PRIVATE
    3rdparty/libuv/include
)

# ============================================
# flv2moq_client（FLV 文件 → MOQT 推流工具）
# 依赖上面的 webtransport_client 静态库
# ============================================
add_subdirectory(tools/flv2moq_client)

# ============================================
# Media over WebTransport（C++ 服务端）
# ============================================
add_subdirectory(moqlivecast)
