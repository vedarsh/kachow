# transport/CMakeLists.txt

# Public headers for transport users (usrl_net.h)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/tcp/includes)

add_library(usrl_net STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/tcp/src/usrl_net_common.c
    ${CMAKE_CURRENT_SOURCE_DIR}/tcp/src/tcp_transport.c
)

target_link_libraries(usrl_net PUBLIC usrl_core)
