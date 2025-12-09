# transport/CMakeLists.txt

add_library(usrl_net STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/usrl_net_common.c
    ${CMAKE_CURRENT_SOURCE_DIR}/tcp/src/usrl_tcp.c
    ${CMAKE_CURRENT_SOURCE_DIR}/udp/src/usrl_udp.c
)

target_include_directories(usrl_net PUBLIC 
    ${CMAKE_CURRENT_SOURCE_DIR}/tcp/includes
    ${CMAKE_CURRENT_SOURCE_DIR}/udp/includes
    ${CMAKE_CURRENT_SOURCE_DIR}
)


target_link_libraries(usrl_net PUBLIC usrl_core)
