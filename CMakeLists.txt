cmake_minimum_required(VERSION 3.20)
project(optionsdesk LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(OD_NATIVE "Tune for the build machine (-march=native)" ON)
option(OD_TESTS "Build tests" ON)

include(FetchContent)
FetchContent_Declare(json
  URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(json)

find_package(CURL REQUIRED)
find_package(Threads REQUIRED)

add_library(od_core
  src/implied_vol.cpp
  src/svi.cpp
  src/chain.cpp
  src/surface.cpp
  src/market_data.cpp)
target_include_directories(od_core PUBLIC include)
target_compile_options(od_core PUBLIC -Wall -Wextra -Wpedantic)
if(OD_NATIVE)
  target_compile_options(od_core PUBLIC -march=native)
endif()

# Exchange connectivity kept out of od_core so pricing/vol code has no I/O deps.
add_library(od_deribit src/deribit.cpp)
target_link_libraries(od_deribit PUBLIC od_core nlohmann_json::nlohmann_json CURL::libcurl)

add_executable(od_surface apps/od_surface.cpp)
target_link_libraries(od_surface PRIVATE od_deribit)

add_executable(od_bench bench/od_bench.cpp)
target_link_libraries(od_bench PRIVATE od_core)

if(OD_TESTS)
  FetchContent_Declare(googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  FetchContent_MakeAvailable(googletest)
  enable_testing()
  add_executable(od_tests
    tests/test_black76.cpp
    tests/test_implied_vol.cpp
    tests/test_svi.cpp
    tests/test_chain.cpp
    tests/test_spsc_ring.cpp
    tests/test_histogram.cpp)
  target_link_libraries(od_tests PRIVATE od_deribit Threads::Threads GTest::gtest_main)
  target_compile_definitions(od_tests PRIVATE OD_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")
  include(GoogleTest)
  gtest_discover_tests(od_tests)
endif()
