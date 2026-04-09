cmake_minimum_required(VERSION 3.16)
project(trading-engine)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Platform detection
if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
    message(STATUS "Platform: ARM64 (Apple M)")
    set(ARCH_FLAGS "-march=armv8-a")
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
    message(STATUS "Platform: x86_64")
    set(ARCH_FLAGS "-march=native")
else()
    set(ARCH_FLAGS "")
endif()

set(CMAKE_CXX_FLAGS         "${CMAKE_CXX_FLAGS} -Wall -Wextra")
set(CMAKE_CXX_FLAGS_RELEASE "-O3 ${ARCH_FLAGS}")
set(CMAKE_CXX_FLAGS_DEBUG   "-O0 -g -fsanitize=address")

include_directories(include)

# Tests
add_executable(trading-engine src/main.cpp
        include/PriceLevelBook.h
        include/SPSCQueue.h)

# SPSC Queue Tests
add_executable(test_spsc src/test_spsc.cpp)
target_link_libraries(test_spsc PRIVATE pthread)

# Benchmark
add_executable(bench bench/bench.cpp
        bench/bench.cpp
        include/Timer.h
        include/PriceLevelBook.h
        include/ObjectPool.h
        include/SPSCQueue.h)
target_compile_options(bench PRIVATE -O3)