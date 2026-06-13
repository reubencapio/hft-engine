cmake_minimum_required(VERSION 3.20)
project(hft-engine VERSION 1.0.0 LANGUAGES CXX)

# ─── C++20 Standard ──────────────────────────────────────────────────────────
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ─── Build Options ────────────────────────────────────────────────────────────
option(HFT_USE_MPI       "Build with OpenMPI support"       OFF)
option(HFT_USE_QT        "Build Qt GUI dashboard"           OFF)
option(HFT_BUILD_TESTS   "Build Google Test suite"          ON)
option(HFT_USE_PROMETHEUS "Build with Prometheus metrics"   OFF)

# ─── Compiler Flags ──────────────────────────────────────────────────────────
add_compile_options(-Wall -Wextra -Wpedantic -Wno-unused-parameter)

if(CMAKE_BUILD_TYPE STREQUAL "Release")
    add_compile_options(-O3 -march=native -DNDEBUG)
elseif(CMAKE_BUILD_TYPE STREQUAL "Debug")
    add_compile_options(-g -O0 -fsanitize=address -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address)
elseif(CMAKE_BUILD_TYPE STREQUAL "TSan")
    # ThreadSanitizer — detects data races. Use OMP_NUM_THREADS=1 when
    # running tests to avoid false positives from the OpenMP runtime.
    add_compile_options(-g -O1 -fsanitize=thread -fno-omit-frame-pointer)
    add_link_options(-fsanitize=thread)
elseif(CMAKE_BUILD_TYPE STREQUAL "UBSan")
    # UndefinedBehaviorSanitizer — catches signed overflow, null deref, etc.
    add_compile_options(-g -O1 -fsanitize=undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=undefined)
elseif(CMAKE_BUILD_TYPE STREQUAL "Coverage")
    # gcov instrumentation for lcov/gcovr coverage reports.
    add_compile_options(-g -O0 --coverage -fprofile-arcs -ftest-coverage)
    add_link_options(--coverage)
elseif(CMAKE_BUILD_TYPE STREQUAL "Valgrind")
    # Plain debug build — no sanitizers so Valgrind can attach cleanly.
    add_compile_options(-g -O0 -fno-omit-frame-pointer)
endif()

# ─── OpenMP ──────────────────────────────────────────────────────────────────
find_package(OpenMP REQUIRED)

# ─── NUMA (optional) ─────────────────────────────────────────────────────────
find_library(NUMA_LIB numa)
if(NUMA_LIB)
    message(STATUS "Found libnuma: ${NUMA_LIB}")
    add_compile_definitions(HFT_HAS_NUMA)
else()
    message(STATUS "libnuma not found — NUMA-aware allocation disabled")
endif()

# ─── Threads ─────────────────────────────────────────────────────────────────
find_package(Threads REQUIRED)

# ─── MPI (optional) ──────────────────────────────────────────────────────────
if(HFT_USE_MPI)
    find_package(MPI REQUIRED)
    add_compile_definitions(HFT_USE_MPI)
    message(STATUS "MPI enabled: ${MPI_CXX_COMPILER}")
endif()

# ═══════════════════════════════════════════════════════════════════════════════
# Core Library
# ═══════════════════════════════════════════════════════════════════════════════
add_library(hft_core STATIC
    src/core/order_book.cpp
    src/core/matching_engine.cpp
    src/feed/itch_parser.cpp
    src/feed/feed_simulator.cpp
    src/strategy/market_maker.cpp
    src/strategy/momentum.cpp
    src/backtest/backtest_engine.cpp
    src/backtest/parameter_sweep.cpp
    src/backtest/mpi_coordinator.cpp
)

target_include_directories(hft_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(hft_core PUBLIC
    Threads::Threads
    OpenMP::OpenMP_CXX
)

if(NUMA_LIB)
    target_link_libraries(hft_core PUBLIC ${NUMA_LIB})
endif()

# ─── MPI Coordinator (optional) ─────────────────────────────────────────────
if(HFT_USE_MPI)
    target_link_libraries(hft_core PUBLIC MPI::MPI_CXX)

    add_library(hft_mpi STATIC
        src/backtest/mpi_coordinator.cpp
    )
    target_include_directories(hft_mpi PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(hft_mpi PUBLIC hft_core MPI::MPI_CXX)
endif()

# ─── Prometheus Metrics (optional) ──────────────────────────────────────────
if(HFT_USE_PROMETHEUS)
    add_library(hft_metrics STATIC
        src/metrics/prometheus_metrics.cpp
    )
    target_include_directories(hft_metrics PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(hft_metrics PUBLIC Threads::Threads)
    add_compile_definitions(HFT_USE_PROMETHEUS)
endif()

# ═══════════════════════════════════════════════════════════════════════════════
# Main Executable
# ═══════════════════════════════════════════════════════════════════════════════
add_executable(hft-engine src/main.cpp)
target_link_libraries(hft-engine PRIVATE hft_core)

if(HFT_USE_MPI)
    target_link_libraries(hft-engine PRIVATE hft_mpi)
endif()

if(HFT_USE_PROMETHEUS)
    target_link_libraries(hft-engine PRIVATE hft_metrics)
endif()

# ═══════════════════════════════════════════════════════════════════════════════
# Qt Dashboard (optional)
# ═══════════════════════════════════════════════════════════════════════════════
if(HFT_USE_QT)
    add_subdirectory(gui)
endif()

# ═══════════════════════════════════════════════════════════════════════════════
# Google Tests
# ═══════════════════════════════════════════════════════════════════════════════
if(HFT_BUILD_TESTS)
    enable_testing()
    find_package(GTest REQUIRED)

    # SPSC Queue test
    add_executable(test_spsc_queue tests/test_spsc_queue.cpp)
    target_include_directories(test_spsc_queue PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(test_spsc_queue PRIVATE GTest::gtest GTest::gtest_main Threads::Threads)
    add_test(NAME SPSCQueueTest COMMAND test_spsc_queue)

    # Order Book test
    add_executable(test_order_book tests/test_order_book.cpp)
    target_link_libraries(test_order_book PRIVATE hft_core GTest::gtest GTest::gtest_main)
    add_test(NAME OrderBookTest COMMAND test_order_book)

    # Matching Engine test
    add_executable(test_matching_engine tests/test_matching_engine.cpp)
    target_link_libraries(test_matching_engine PRIVATE hft_core GTest::gtest GTest::gtest_main)
    add_test(NAME MatchingEngineTest COMMAND test_matching_engine)

    # Parameter Sweep test
    add_executable(test_parameter_sweep tests/test_parameter_sweep.cpp)
    target_link_libraries(test_parameter_sweep PRIVATE hft_core GTest::gtest GTest::gtest_main)
    add_test(NAME ParameterSweepTest COMMAND test_parameter_sweep)
endif()
