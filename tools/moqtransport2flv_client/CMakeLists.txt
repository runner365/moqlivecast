# ============================================
# moqtransport2flv_client — subscribe with the C SDK, mux to FLV
# Uses moqlivecast/format/flv/FlvMuxer for FLV writing.
# ============================================

set(MOQLIVECAST_DIR ${CMAKE_SOURCE_DIR}/moqlivecast)

add_executable(moqtransport2flv_client
    main.cpp
    ${MOQLIVECAST_DIR}/format/flv/flv_mux.cpp
    ${MOQLIVECAST_DIR}/format/flv/flv_pub.cpp
    ${MOQLIVECAST_DIR}/format/h264_h265_header.cpp
)

set_target_properties(moqtransport2flv_client PROPERTIES
    CXX_STANDARD 14
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

target_link_libraries(moqtransport2flv_client PRIVATE
    moq_transport_client
)

target_include_directories(moqtransport2flv_client PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${MOQLIVECAST_DIR}
    ${MOQLIVECAST_DIR}/format
    ${MOQLIVECAST_DIR}/format/flv
    ${MOQLIVECAST_DIR}/format/amf
    ${MOQLIVECAST_DIR}/utils
    ${MOQLIVECAST_DIR}/utils/av
)
