# transport/CMakeLists.txt

add_library(usrl_net STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/tcp/src/usrl_net_common.c
    ${CMAKE_CURRENT_SOURCE_DIR}/tcp/src/tcp_transport.c
)

target_include_directories(usrl_net PUBLIC 
    ${CMAKE_CURRENT_SOURCE_DIR}/tcp/includes
)

target_link_libraries(usrl_net PUBLIC usrl_core)
