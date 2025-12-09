# Do NOT call project() here; top-level CMakeLists already did that.

# Tools share the core library's public includes; nothing extra needed
# unless tools have their own local headers.

# usrl-ctl tool
add_executable(multi_publisher
    multi_publisher.c
)
target_link_libraries(multi_publisher PRIVATE usrl_core pthread)

add_executable(market_publisher
    market_publisher.c
)
target_link_libraries(market_publisher PRIVATE usrl_core pthread)
add_executable(api_test
    api_test.c
)
target_link_libraries(api_test PRIVATE usrl_core pthread)

add_executable(soak_test
    soak_test.c
)
target_link_libraries(soak_test PRIVATE usrl_core pthread)