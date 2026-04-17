set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_COMPILER g++)
set(CMAKE_CXX_FLAGS "-std=c++2a -Wall -Wextra -Werror -Wpedantic")
set(CMAKE_VERBOSE_MAKEFILE on)

file(GLOB SOURCES "*/*.cpp")

include_directories(${PROJECT_SOURCE_DIR})
include_directories(${PROJECT_SOURCE_DIR}/exchange)

add_library(libexchange STATIC ${SOURCES})

# Release variant: same sources, but with hot-path logging compiled out and
# optimization flags on. Used only by latency_benchmark_release.
add_library(libexchange_release STATIC ${SOURCES})
target_compile_definitions(libexchange_release PRIVATE NT_BENCHMARK_NO_LOG)
target_compile_options(libexchange_release PRIVATE
    -O3 -DNDEBUG -march=native -fno-omit-frame-pointer)
