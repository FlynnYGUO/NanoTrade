cmake_minimum_required(VERSION 3.0)

project(LowLatencyApp)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_COMPILER g++)
set(CMAKE_CXX_FLAGS "-std=c++2a -Wall -Wextra -Werror -Wpedantic")
set(CMAKE_VERBOSE_MAKEFILE on)

add_subdirectory(common)
add_subdirectory(exchange)
add_subdirectory(trading)
add_subdirectory(benchmarks)
add_subdirectory(backtest)

list(APPEND LIBS libbacktest)
list(APPEND LIBS libexchange)
list(APPEND LIBS libtrading)
list(APPEND LIBS libcommon)
list(APPEND LIBS pthread)

# Release link list: swaps libexchange for libexchange_release (hot-path log stripped + -O3).
list(APPEND LIBS_RELEASE libexchange_release)
list(APPEND LIBS_RELEASE libtrading)
list(APPEND LIBS_RELEASE libcommon)
list(APPEND LIBS_RELEASE pthread)

include_directories(${PROJECT_SOURCE_DIR})
include_directories(${PROJECT_SOURCE_DIR}/exchange)
include_directories(${PROJECT_SOURCE_DIR}/trading)

add_executable(exchange_main exchange/exchange_main.cpp)
target_link_libraries(exchange_main PUBLIC ${LIBS})

add_executable(trading_main trading/trading_main.cpp)
target_link_libraries(trading_main PUBLIC ${LIBS})

add_executable(logger_benchmark benchmarks/logger_benchmark.cpp)
target_link_libraries(logger_benchmark PUBLIC ${LIBS})

add_executable(release_benchmark benchmarks/release_benchmark.cpp)
target_link_libraries(release_benchmark PUBLIC ${LIBS})

add_executable(hash_benchmark benchmarks/hash_benchmark.cpp)
target_link_libraries(hash_benchmark PUBLIC ${LIBS})

add_executable(latency_benchmark benchmarks/latency_benchmark.cpp)
target_link_libraries(latency_benchmark PUBLIC ${LIBS})

add_executable(backtest_main backtest/backtest_main.cpp)
target_link_libraries(backtest_main PUBLIC ${LIBS})

# Release build of latency_benchmark with hot-path logging compiled out.
# NT_BENCHMARK_NO_LOG neutralizes NT_LOG, END_MEASURE, and TTT_MEASURE so the
# matching engine runs without logger_.log() calls on the order-processing path.
# Links against libexchange_release so the ME / OB .cpp code is also stripped of logging.
add_executable(latency_benchmark_release benchmarks/latency_benchmark.cpp)
target_link_libraries(latency_benchmark_release PUBLIC ${LIBS_RELEASE})
target_compile_options(latency_benchmark_release PRIVATE
    -O3 -DNDEBUG -march=native -fno-omit-frame-pointer)
target_compile_definitions(latency_benchmark_release PRIVATE NT_BENCHMARK_NO_LOG)
