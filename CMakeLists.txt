cmake_minimum_required(VERSION 3.16)
project(usrl_core C)

set(CMAKE_C_STANDARD 11)

# -------------------------------------------------------------
# GLOBAL INCLUDE DIRS
# -------------------------------------------------------------
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/core/includes
)

# -------------------------------------------------------------
# CORE LIBRARY
# -------------------------------------------------------------
add_library(usrl_core
    core/src/usrl_core.c
    core/src/ring_swmr.c
)

find_package(Threads REQUIRED)
target_link_libraries(usrl_core PUBLIC Threads::Threads rt atomic)

# -------------------------------------------------------------
# EXECUTABLES
# -------------------------------------------------------------
add_executable(init_core main_init_core.c)
target_link_libraries(init_core usrl_core)

add_executable(pub modules/publisher_demo.c)
target_link_libraries(pub usrl_core)

add_executable(sub modules/subscriber_demo.c)
target_link_libraries(sub usrl_core)
