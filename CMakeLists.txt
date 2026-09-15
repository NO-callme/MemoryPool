cmake_minimum_required(VERSION 3.10)
project(mempool CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_compile_options(-Wall -Wextra -O2)

find_package(Threads REQUIRED)

# 目录
set(SRC_DIR ${CMAKE_SOURCE_DIR}/src)
set(INC_DIR ${CMAKE_SOURCE_DIR}/include)
set(TEST_DIR ${CMAKE_SOURCE_DIR}/tests)

# 一把梭：src 下所有 cpp 编进同一个可执行文件（与 v2 一致的框架）
file(GLOB SOURCES "${SRC_DIR}/*.cpp")
include_directories(${INC_DIR})

add_executable(unit_test
    ${SOURCES}
    ${TEST_DIR}/UnitTest.cpp
)
add_executable(perf_test
    ${SOURCES}
    ${TEST_DIR}/PerformanceTest.cpp
)

target_link_libraries(unit_test PRIVATE Threads::Threads)
target_link_libraries(perf_test PRIVATE Threads::Threads)

add_custom_target(test
    COMMAND ./unit_test
    DEPENDS unit_test
)
add_custom_target(perf
    COMMAND ./perf_test
    DEPENDS perf_test
)
