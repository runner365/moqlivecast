# ============================================
# flv2moq_client — 读取 FLV 文件，按时间戳以 MOQT 推送到 moqlivecast
#
# 依赖 webtransport_client 静态库（根 CMakeLists.txt 定义）提供 QUIC/WT 客户端。
# 复用服务端的 FlvDemuxer 做解复用 —— 只需 4 个 .cpp，且不牵入 moqlivecast
# 的服务端逻辑（已确认无反向依赖）。
# ============================================

set(MOQLIVECAST_DIR ${CMAKE_SOURCE_DIR}/moqlivecast)

add_executable(flv2moq_client
    main.cpp
    flv_reader.cpp
    moq_wire.cpp
    moq_publisher.cpp

    # FlvDemuxer 及其直接依赖（header-only 的 logger/data_buffer/av 等无需列出）
    ${MOQLIVECAST_DIR}/format/flv/flv_demux.cpp
    ${MOQLIVECAST_DIR}/format/flv/flv_pub.cpp
    ${MOQLIVECAST_DIR}/format/h264_h265_header.cpp
    ${MOQLIVECAST_DIR}/format/audio_header.cpp
)

set_target_properties(flv2moq_client PROPERTIES
    CXX_STANDARD 14
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

# webtransport_client 的 PUBLIC 依赖（uv_a/ssl/crypto）与 include 会自动传递
target_link_libraries(flv2moq_client PRIVATE webtransport_client)

target_include_directories(flv2moq_client PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${MOQLIVECAST_DIR}
    ${MOQLIVECAST_DIR}/format
    ${MOQLIVECAST_DIR}/format/flv
    ${MOQLIVECAST_DIR}/format/amf
    ${MOQLIVECAST_DIR}/utils
    ${MOQLIVECAST_DIR}/utils/av
)
