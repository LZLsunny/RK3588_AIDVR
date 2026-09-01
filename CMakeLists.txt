cmake_minimum_required(VERSION 3.16)

project(aimedia LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(CTest)
enable_testing()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/cmake")
include(RockchipSdk)

add_executable(hello_rk3588 src/test.c)
add_executable(isp_bridge_test src/isp_bridge_test.c)

add_executable(smoke_test tests/unit/smoke_test.cpp)

find_package(GTest QUIET)
if(GTest_FOUND)
  target_compile_definitions(smoke_test PRIVATE AIMEDIA_HAS_GTEST=1)
  target_link_libraries(smoke_test PRIVATE GTest::gtest GTest::gtest_main)
endif()

add_test(NAME BuildContract.UsesCxx17 COMMAND smoke_test)
