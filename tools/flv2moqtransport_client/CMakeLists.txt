# ============================================
# flv2moqtransport_client — FLV file → moq-transport publish
# Uses the C SDK in moq_transport_client/ and the server FlvDemuxer.
# ============================================

set(MOQLIVECAST_DIR ${CMAKE_SOURCE_DIR}/moqlivecast)

add_executable(flv2moqtransport_client
    main.cpp
    ../flv2moq_client/flv_reader.cpp

    ${MOQLIVECAST_DIR}/format/flv/flv_demux.cpp
    ${MOQLIVECAST_DIR}/format/flv/flv_pub.cpp
    ${MOQLIVECAST_DIR}/format/h264_h265_header.cpp
    ${MOQLIVECAST_DIR}/format/audio_header.cpp
)

set_target_properties(flv2moqtransport_client PROPERTIES
    CXX_STANDARD 14
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

target_link_libraries(flv2moqtransport_client PRIVATE
    moq_transport_client
)

target_include_directories(flv2moqtransport_client PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/tools/flv2moq_client
    ${MOQLIVECAST_DIR}
    ${MOQLIVECAST_DIR}/format
    ${MOQLIVECAST_DIR}/format/flv
    ${MOQLIVECAST_DIR}/format/amf
    ${MOQLIVECAST_DIR}/utils
    ${MOQLIVECAST_DIR}/utils/av
)
