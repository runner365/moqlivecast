# ============================================
# flv2moqlite_client — FLV 文件 → moq-lite-06 推流（C++）
#
# 目的：**验证 moqlite 的 C API**（moqlite/moq_lite_client.{h,c}）。与
# flv2moq_client 的区别是协议：那个走 draft-ietf-moq-transport + WebTransport，
# 这个走 moq-lite-06 裸 QUIC（就是 moq-cli 用的那条路）。
#
# FLV 解封装**复用**，不重写 —— 与 flv2moq_client 一样直接用服务端那份
# FlvDemuxer（4 个 .cpp，无反向依赖），外加它已经封好的 flv_reader.{hpp,cpp}
# （剥 5B/2B tag 头、过滤 H.264/AAC、给出 is_seq_hdr/key/dts/cts/payload）。
# ============================================

set(MOQLIVECAST_DIR ${CMAKE_SOURCE_DIR}/moqlivecast)
set(FLV2MOQ_DIR ${CMAKE_SOURCE_DIR}/tools/flv2moq_client)

add_executable(flv2moqlite_client
    main.cpp

    # 复用 flv2moq_client 的解封装层（FlvReader 已经封好 tag 头的剥离）
    ${FLV2MOQ_DIR}/flv_reader.cpp

    # FlvDemuxer 及其直接依赖（与 flv2moq_client/CMakeLists.txt 同一批）
    ${MOQLIVECAST_DIR}/format/flv/flv_demux.cpp
    ${MOQLIVECAST_DIR}/format/flv/flv_pub.cpp
    ${MOQLIVECAST_DIR}/format/h264_h265_header.cpp
    ${MOQLIVECAST_DIR}/format/audio_header.cpp
)

set_target_properties(flv2moqlite_client PROPERTIES
    CXX_STANDARD 14
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

# moqlite_client_core 提供 C API + quic_client_api + QUIC/TLS/UDP/logger；
# 它的 PUBLIC 依赖（uv_a/ssl/crypto）与 include 会自动传递过来。
target_link_libraries(flv2moqlite_client PRIVATE moqlite_client_core)

target_include_directories(flv2moqlite_client PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${FLV2MOQ_DIR}                 # flv_reader.hpp
    ${MOQLIVECAST_DIR}
    ${MOQLIVECAST_DIR}/format
    ${MOQLIVECAST_DIR}/format/flv
    ${MOQLIVECAST_DIR}/format/amf
    ${MOQLIVECAST_DIR}/utils
    ${MOQLIVECAST_DIR}/utils/av
    ${CMAKE_SOURCE_DIR}/moqlite    # moq_lite_client.h
    ${CMAKE_SOURCE_DIR}/logger     # logger.h（C 侧）
    ${CMAKE_SOURCE_DIR}/udp        # data_buffer/udp 头
    ${CMAKE_SOURCE_DIR}/tls
    ${CMAKE_SOURCE_DIR}/quic
    ${CMAKE_SOURCE_DIR}/quic/congestion_control
)
