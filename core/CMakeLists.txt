# core/CMakeLists.txt

# Export includes so sub-projects find headers automatically
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/includes)

# --- Library Definition ---
add_library(usrl_core SHARED
    src/usrl_core.c
    src/ring_swmr.c
    src/ring_mwmr.c
    src/usrl_health.c
    src/usrl_backpressure.c
    src/usrl_logging.c
    src/usrl_schema.c
    src/usrl.c
)

# Link required system libs
find_package(Threads REQUIRED)

# Always link pthreads
target_link_libraries(usrl_core PUBLIC Threads::Threads)

# Link rt and atomic only on non-Apple (Linux, etc.)
if(NOT APPLE)
    target_link_libraries(usrl_core PUBLIC rt atomic)
endif()

# Expose library and headers to other dirs (benchmarks/tools)
target_include_directories(usrl_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/includes
)
