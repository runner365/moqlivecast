# ============================================
# moq_transport_client — C SDK for moq-transport
# (H.264 + AAC publish / subscribe over WebTransport)
# ============================================

add_library(moq_transport_client STATIC
    src/moq_transport_client.c
)

set_target_properties(moq_transport_client PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    POSITION_INDEPENDENT_CODE ON
)

target_include_directories(moq_transport_client PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# webtransport_client already exports uv_a / ssl / crypto and the
# quic/http3/tls/udp include directories as PUBLIC dependencies.
target_link_libraries(moq_transport_client PUBLIC webtransport_client)

add_executable(moq_transport_client_smoke
    tests/moq_transport_client_smoke.c
)
set_target_properties(moq_transport_client_smoke PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)
target_link_libraries(moq_transport_client_smoke PRIVATE moq_transport_client)
