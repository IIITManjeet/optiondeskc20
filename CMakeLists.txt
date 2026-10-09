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

# Hot-path JSON parsing in the feed (On-Demand API: no DOM, reads only the fields used).
set(SIMDJSON_DEVELOPER_MODE OFF CACHE INTERNAL "")
FetchContent_Declare(simdjson
  URL https://github.com/simdjson/simdjson/archive/refs/tags/v3.10.1.tar.gz
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(simdjson)

find_package(CURL REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)
find_package(Boost 1.74 REQUIRED CONFIG COMPONENTS headers)

add_library(od_core
  src/implied_vol.cpp
  src/svi.cpp
  src/chain.cpp
  src/surface.cpp
  src/market_data.cpp
  src/risk.cpp
  src/orders.cpp
  src/quoter.cpp
  src/risk_gate.cpp
  src/gateway.cpp)
target_include_directories(od_core PUBLIC include)
target_compile_options(od_core PUBLIC -Wall -Wextra -Wpedantic)
if(OD_NATIVE)
  target_compile_options(od_core PUBLIC -march=native)
endif()

# Exchange connectivity kept out of od_core so pricing/vol code has no I/O deps.
add_library(od_deribit src/deribit.cpp src/portfolio_io.cpp)
target_link_libraries(od_deribit PUBLIC od_core nlohmann_json::nlohmann_json CURL::libcurl)

# Streaming feed: Boost.Beast WebSocket over OpenSSL.
add_library(od_feed src/deribit_feed.cpp src/ticker_parser.cpp src/feed_bus.cpp src/journal.cpp
  src/mm_engine.cpp)
target_link_libraries(od_feed PUBLIC od_deribit Boost::headers OpenSSL::SSL OpenSSL::Crypto Threads::Threads
  PRIVATE simdjson::simdjson)

add_executable(od_surface apps/od_surface.cpp)
target_link_libraries(od_surface PRIVATE od_deribit)

add_executable(od_live apps/od_live.cpp)
target_link_libraries(od_live PRIVATE od_feed)

add_executable(od_feedd apps/od_feedd.cpp)
target_link_libraries(od_feedd PRIVATE od_feed)

add_executable(od_mm apps/od_mm.cpp)
target_link_libraries(od_mm PRIVATE od_feed)

add_executable(od_record apps/od_record.cpp)
target_link_libraries(od_record PRIVATE od_feed)

add_executable(od_replay apps/od_replay.cpp)
target_link_libraries(od_replay PRIVATE od_feed)

add_executable(od_risk apps/od_risk.cpp)
target_link_libraries(od_risk PRIVATE od_feed)

add_executable(od_bench bench/od_bench.cpp)
target_link_libraries(od_bench PRIVATE od_feed)
target_compile_definitions(od_bench PRIVATE OD_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")

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
    tests/test_histogram.cpp
    tests/test_ticker_parser.cpp
    tests/test_risk.cpp
    tests/test_feed_bus.cpp
    tests/test_mm.cpp
    tests/test_journal.cpp)
  target_link_libraries(od_tests PRIVATE od_feed GTest::gtest_main)
  target_compile_definitions(od_tests PRIVATE OD_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")
  include(GoogleTest)
  gtest_discover_tests(od_tests)
endif()
