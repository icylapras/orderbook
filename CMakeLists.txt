cmake_minimum_required(VERSION 3.20)
project(orderbook LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

option(ORDERBOOK_NATIVE "Tune Release builds for this CPU (-march=native)" ON)
set(ORDERBOOK_SANITIZE "" CACHE STRING "Sanitizers to build with, e.g. address,undefined or thread")
option(ORDERBOOK_FUZZ "Build the libFuzzer ITCH parser target (clang only)" OFF)
option(ORDERBOOK_FETCH_GTEST "Always build GoogleTest from source (needed for sanitizer builds)" OFF)

add_compile_options(-Wall -Wextra -Wpedantic)
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG")
if(ORDERBOOK_NATIVE)
    string(APPEND CMAKE_CXX_FLAGS_RELEASE " -march=native")
endif()

if(ORDERBOOK_SANITIZE)
    add_compile_options(-fsanitize=${ORDERBOOK_SANITIZE} -fno-omit-frame-pointer -fno-sanitize-recover=all)
    add_link_options(-fsanitize=${ORDERBOOK_SANITIZE})
endif()

# ---------------------------------------------------------------- engine
add_library(orderbook_core
    src/Orderbook.cpp
    src/FastOrderbook.cpp)
target_include_directories(orderbook_core PUBLIC src)
find_package(Threads REQUIRED)
target_link_libraries(orderbook_core PUBLIC Threads::Threads)

# ---------------------------------------------------------------- tools
add_executable(demo tools/demo.cpp)
target_link_libraries(demo PRIVATE orderbook_core)

add_executable(replay tools/replay.cpp)
target_link_libraries(replay PRIVATE orderbook_core)
# replay replaces global operator new (to count allocations); once gcc inlines
# that malloc-based new into libstdc++ it reports bogus -Wstringop-overflow writes
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_options(replay PRIVATE -Wno-stringop-overflow)
endif()

add_executable(itchgen tools/itchgen.cpp)
target_link_libraries(itchgen PRIVATE orderbook_core)

add_executable(itchfilter tools/itchfilter.cpp)
target_link_libraries(itchfilter PRIVATE orderbook_core)

# ---------------------------------------------------------------- benchmarks
add_executable(bench_engine bench/benchmark.cpp)
target_link_libraries(bench_engine PRIVATE orderbook_core)

add_executable(bench_queue bench/queue_bench.cpp)
target_link_libraries(bench_queue PRIVATE orderbook_core)

# ---------------------------------------------------------------- tests
enable_testing()

if(NOT ORDERBOOK_FETCH_GTEST AND NOT ORDERBOOK_SANITIZE)
    find_package(GTest QUIET)
endif()
if(NOT GTest_FOUND)
    include(FetchContent)
    FetchContent_Declare(googletest
        URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz
        DOWNLOAD_EXTRACT_TIMESTAMP ON)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
    if(NOT TARGET GTest::gtest_main)
        add_library(GTest::gtest_main ALIAS gtest_main)
    endif()
endif()

add_executable(unit_tests
    tests/test.cpp
    tests/property_test.cpp
    tests/itch_test.cpp
    tests/components_test.cpp
    tests/concurrency_test.cpp)
target_link_libraries(unit_tests PRIVATE orderbook_core GTest::gtest_main)
target_compile_definitions(unit_tests PRIVATE ORDERBOOK_TEST_FILES="${CMAKE_CURRENT_SOURCE_DIR}/tests/TestFiles")

include(GoogleTest)
gtest_discover_tests(unit_tests DISCOVERY_TIMEOUT 60)

# ---------------------------------------------------------------- fuzzing
if(ORDERBOOK_FUZZ)
    add_executable(fuzz_itch fuzz/itch_fuzz.cpp)
    target_link_libraries(fuzz_itch PRIVATE orderbook_core)
    target_compile_options(fuzz_itch PRIVATE -fsanitize=fuzzer)
    target_link_options(fuzz_itch PRIVATE -fsanitize=fuzzer)
endif()
