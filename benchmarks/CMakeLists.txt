# Benchmarks use the parent 'usrl_core' target
project(usrl_benchmarks)

# 1. Init Bench Tool
add_executable(init_bench init_bench.c)
target_link_libraries(init_bench usrl_core)

# 2. SWMR Pub
add_executable(bench_pub_swmr bench_pub_swmr.c)
target_link_libraries(bench_pub_swmr usrl_core)

# 3. MWMR Pub
add_executable(bench_pub_mwmr bench_pub_mwmr.c)
target_link_libraries(bench_pub_mwmr usrl_core)

# 4. Subscriber
add_executable(bench_sub bench_sub.c)
target_link_libraries(bench_sub usrl_core)

# 2. TCP Benchmarks (need usrl_net headers + libs)
add_executable(bench_tcp_server bench_tcp_server.c)
target_link_libraries(bench_tcp_server usrl_net usrl_core)
# Automatically gets transport/includes from usrl_net target

add_executable(bench_tcp_client bench_tcp_client.c)
target_link_libraries(bench_tcp_client usrl_net usrl_core)

# Copy the config JSON to the build directory
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/usrl_config_bench.json
    ${CMAKE_CURRENT_BINARY_DIR}/usrl_config_bench.json
    COPYONLY
)