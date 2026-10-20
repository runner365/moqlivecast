# ============================================================
# Android 侧的 native 构建入口 —— 只编「推流用的那一小块」。
#
# 刻意**不复用**仓库根那本 CMakeLists：那本会牵进 moqlivecast（C++ 服务端）、
# http3、各种测试程序，全都不该出现在 APK 里；而且它的 OpenSSL target 选择在
# Android 下是错的（Android 在 CMake 眼里也是 UNIX → 会挑 linux-aarch64，链接
# 出一个跑不到 Bionic 上的 libcrypto）。这里显式列出需要的源文件，宁可重复。
#
# 需要的东西：
#   libuv        —— add_subdirectory，libuv 自己带 Android 分支
#   OpenSSL      —— 预编译好的静态库（tools/android/build_openssl_android.sh）
#   moqlite/ + quic/ + tls/ + udp/ + logger/  —— 纯 C，直接编
#
# Gradle 传进来的变量：
#   MOQ_ROOT    —— 仓库根（app/build.gradle.kts 里算好传进来）
#   ANDROID_ABI —— AGP 提供
# ============================================================
cmake_minimum_required(VERSION 3.22)
project(moqlitepush C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT MOQ_ROOT)
    message(FATAL_ERROR "MOQ_ROOT is not set — pass -DMOQ_ROOT=<repo root> from Gradle.")
endif()
if(NOT ANDROID_ABI)
    message(FATAL_ERROR "ANDROID_ABI is not set — this CMakeLists is meant to be driven by AGP.")
endif()

# ⚠️ 别用 `../../..` 去数层级：本文件在 `app/src/main/cpp`，要**四级**才到
# tools/android —— 数错一级就会去找 `tools/android/app/3rdparty/...`，报出来
# 是「OpenSSL 找不到」，看着像脚本没跑，其实是路径算错。直接从 Gradle 传进来
# 的 MOQ_ROOT 拼，不会漂。
set(MOQ_ANDROID_DIR "${MOQ_ROOT}/tools/android")
set(OPENSSL_DIR "${MOQ_ANDROID_DIR}/3rdparty/openssl/${ANDROID_ABI}")

# OpenSSL 是脚本预编译的，不在这里编。找不到就**明确报错并给出补救命令** ——
# 否则会在链接期报一大堆 `undefined symbol: SSL_...`，指向错误的方向。
if(NOT EXISTS "${OPENSSL_DIR}/lib/libcrypto.a" OR
   NOT EXISTS "${OPENSSL_DIR}/lib/libssl.a")
    message(FATAL_ERROR
        "OpenSSL for ${ANDROID_ABI} not found at ${OPENSSL_DIR}.\n"
        "  MOQ_ROOT        = ${MOQ_ROOT}\n"
        "  MOQ_ANDROID_DIR = ${MOQ_ANDROID_DIR}\n"
        "If either path looks wrong (e.g. it contains `/app/`), the plumbing is\n"
        "broken — not OpenSSL.\n"
        "Otherwise build it first:\n"
        "    bash tools/android/build_openssl_android.sh ${ANDROID_ABI}")
endif()

# ── libuv ────────────────────────────────────────
# 静态即可（uv_a）；共享那份不编，省时间。
set(LIBUV_BUILD_SHARED OFF CACHE BOOL "" FORCE)
add_subdirectory("${MOQ_ROOT}/3rdparty/libuv"
                 "${CMAKE_CURRENT_BINARY_DIR}/libuv" EXCLUDE_FROM_ALL)

# ── OpenSSL（预编译静态库，转成 imported target）─
add_library(ssl    STATIC IMPORTED)
add_library(crypto STATIC IMPORTED)
set_target_properties(ssl    PROPERTIES IMPORTED_LOCATION "${OPENSSL_DIR}/lib/libssl.a")
set_target_properties(crypto PROPERTIES IMPORTED_LOCATION "${OPENSSL_DIR}/lib/libcrypto.a")

# ── moq-lite 客户端（与宿主机的 moqlite_client_core 同一批源文件）──
# ⚠️ 这份清单必须和根 CMakeLists 的 UDP_COMMON_SOURCES / TLS_COMMON_SOURCES /
# QUIC_COMMON_SOURCES 保持一致。加了新文件两边都要改 —— 少一个的症状是链接期
# undefined symbol，不算难查，但别让它漂。
add_library(moqlite_client_core STATIC
    ${MOQ_ROOT}/moqlite/moq_lite_wire.c
    ${MOQ_ROOT}/moqlite/moq_lite_client.c

    ${MOQ_ROOT}/udp/udp_client.c
    ${MOQ_ROOT}/udp/udp_server.c

    ${MOQ_ROOT}/tls/tls_common.c
    ${MOQ_ROOT}/tls/tls_client.c
    ${MOQ_ROOT}/tls/tls_server.c

    ${MOQ_ROOT}/quic/quic_packet.c
    ${MOQ_ROOT}/quic/quic_crypto.c
    ${MOQ_ROOT}/quic/quic_connection.c
    ${MOQ_ROOT}/quic/quic_listener.c
    ${MOQ_ROOT}/quic/quic_stream.c
    ${MOQ_ROOT}/quic/quic_recovery.c
    ${MOQ_ROOT}/quic/quic_timer.c
    ${MOQ_ROOT}/quic/pacing_send.c
    ${MOQ_ROOT}/quic/quic_client_api.c

    ${MOQ_ROOT}/quic/congestion_control/quic_cc_newreno.c
    ${MOQ_ROOT}/quic/congestion_control/quic_cc_bbr.c
    ${MOQ_ROOT}/quic/congestion_control/quic_cc_cubic.c

    ${MOQ_ROOT}/logger/logger.c
)

target_include_directories(moqlite_client_core PUBLIC
    ${MOQ_ROOT}/moqlite
    ${MOQ_ROOT}/udp
    ${MOQ_ROOT}/tls
    ${MOQ_ROOT}/quic
    ${MOQ_ROOT}/quic/congestion_control
    ${MOQ_ROOT}/logger
    ${MOQ_ROOT}/3rdparty/libuv/include
    "${OPENSSL_DIR}/include"
)

target_link_libraries(moqlite_client_core PUBLIC uv_a ssl crypto)
# Android 上 dl 由 libuv 那条链带上（libuv 的 Android 分支已经 APPEND 了）。

# ── JNI 桥 + 推流侧 ─────────────────────────────
#   moq_jni.c      Kotlin 的入口（取数据、转比特流、入队）
#   moq_pusher.c   一条 uv loop 线程 + 跨线程帧队列 + 调 C API
#   annexb_avcc.c  MediaCodec 的 Annex-B → SDK 要的 AVCC，以及 avcC
add_library(moqlitepush SHARED
    moq_jni.c
    moq_pusher.c
    annexb_avcc.c
)
target_link_libraries(moqlitepush PRIVATE moqlite_client_core)

# __android_log_print（moq_pusher.c 里用它写 Logcat）—— liblog 是 Android 的
# 系统库，非 Android 构建下没有这个 target，所以要判一下。
if(ANDROID)
    target_link_libraries(moqlitepush PRIVATE log)
endif()
