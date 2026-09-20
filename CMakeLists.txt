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

file(GLOB SOURCES "${SRC_DIR}/*.cpp")
include_directories(${INC_DIR})

# 单元测试
add_executable(unit_test
    ${SOURCES}
    ${TEST_DIR}/UnitTest.cpp
)
target_include_directories(unit_test PRIVATE tests)
target_link_libraries(unit_test PRIVATE Threads::Threads)

# 基准测试：每个场景一个可执行文件，方便单独跑
foreach(bench bench_single_thread bench_multi_thread bench_realistic)
    add_executable(${bench}
        ${SOURCES}
        ${TEST_DIR}/benchmark/${bench}.cpp
    )
    target_include_directories(${bench} PRIVATE tests)
    target_link_libraries(${bench} PRIVATE Threads::Threads)
endforeach()

# 压力测试
add_executable(stress_test
    ${SOURCES}
    ${TEST_DIR}/stress/stress_test.cpp
)
target_include_directories(stress_test PRIVATE tests)
target_link_libraries(stress_test PRIVATE Threads::Threads)

# 便捷目标
add_custom_target(run_test
    COMMAND ./unit_test
    DEPENDS unit_test
)
add_custom_target(run_stress
    COMMAND ./stress_test
    DEPENDS stress_test
)
add_custom_target(run_bench
    COMMAND ./bench_single_thread
    COMMAND ./bench_multi_thread
    COMMAND ./bench_realistic
    DEPENDS bench_single_thread bench_multi_thread bench_realistic
)

enable_testing()
add_test(NAME unit COMMAND unit_test)
add_test(NAME stress COMMAND stress_test)
