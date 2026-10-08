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

add_library(od_core
  src/implied_vol.cpp)
target_include_directories(od_core PUBLIC include)
target_compile_options(od_core PUBLIC -Wall -Wextra -Wpedantic)
if(OD_NATIVE)
  target_compile_options(od_core PUBLIC -march=native)
endif()

if(OD_TESTS)
  FetchContent_Declare(googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  FetchContent_MakeAvailable(googletest)
  enable_testing()
  add_executable(od_tests
    tests/test_black76.cpp
    tests/test_implied_vol.cpp)
  target_link_libraries(od_tests PRIVATE od_core GTest::gtest_main)
  include(GoogleTest)
  gtest_discover_tests(od_tests)
endif()
