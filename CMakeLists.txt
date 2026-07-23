cmake_minimum_required(VERSION 3.20)
project(MiniTrader VERSION 0.1.0 LANGUAGES CXX)

# Prefer Homebrew packages over conda/system when on macOS
if(APPLE AND EXISTS "/opt/homebrew")
    list(PREPEND CMAKE_PREFIX_PATH "/opt/homebrew")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Compiler flags for performance
add_compile_options(-Wall -Wextra -Wpedantic)
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    add_compile_options(-O3 -march=native -DNDEBUG)
endif()

# Header-only core library
add_library(minitrader_core INTERFACE)
target_include_directories(minitrader_core INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# OrderBook library
add_library(minitrader_orderbook
    src/orderbook/order_book.cpp
)
target_link_libraries(minitrader_orderbook PUBLIC minitrader_core)

# Strategy library
add_library(minitrader_strategy
    src/strategy/strategy_base.cpp
    src/strategy/spread_strategy.cpp
)
target_link_libraries(minitrader_strategy PUBLIC minitrader_core minitrader_orderbook)

# Engine library
add_library(minitrader_engine
    src/engine/trading_engine.cpp
)
target_link_libraries(minitrader_engine PUBLIC
    minitrader_core
    minitrader_orderbook
    minitrader_risk
    minitrader_strategy
)

# Demo executable
add_executable(minitrader_demo src/main.cpp)
target_link_libraries(minitrader_demo PRIVATE minitrader_engine minitrader_strategy)

# Network library (Linux only - epoll)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_library(minitrader_network
        src/network/market_receiver.cpp
    )
    target_link_libraries(minitrader_network PUBLIC minitrader_core)
endif()

# Risk library
add_library(minitrader_risk
    src/risk/risk_gate.cpp
)
target_link_libraries(minitrader_risk PUBLIC minitrader_core)

# ─── Tests ───────────────────────────────────────────────
option(MINITRADER_BUILD_TESTS "Build unit tests" ON)
if(MINITRADER_BUILD_TESTS)
    enable_testing()
    find_package(GTest QUIET)
    if(GTest_FOUND)
        add_executable(test_spsc_queue tests/test_spsc_queue.cpp)
        target_link_libraries(test_spsc_queue minitrader_core GTest::gtest_main)
        add_test(NAME SPSCQueueTest COMMAND test_spsc_queue)

        add_executable(test_orderbook tests/test_orderbook.cpp)
        target_link_libraries(test_orderbook minitrader_orderbook GTest::gtest_main)
        add_test(NAME OrderBookTest COMMAND test_orderbook)
    endif()
endif()

# ─── Benchmarks ──────────────────────────────────────────
option(MINITRADER_BUILD_BENCH "Build benchmarks" ON)
if(MINITRADER_BUILD_BENCH)
    find_package(benchmark QUIET)
    if(benchmark_FOUND)
        add_executable(spsc_bench bench/spsc_bench.cpp)
        target_link_libraries(spsc_bench minitrader_core benchmark::benchmark)

        add_executable(orderbook_bench bench/orderbook_bench.cpp)
        target_link_libraries(orderbook_bench minitrader_orderbook benchmark::benchmark)
    endif()
endif()
