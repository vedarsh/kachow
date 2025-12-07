cmake_minimum_required(VERSION 3.16)
project(usrl_core C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_FLAGS_RELEASE "-O3 -march=native -Wall -Wextra")

add_subdirectory(core)
add_subdirectory(benchmarks)
add_subdirectory(tools)
add_subdirectory(transport)
