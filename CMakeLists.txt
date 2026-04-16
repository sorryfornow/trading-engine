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

# ── Tests ─────────────────────────────────────────────────────────────────────
# Phase 1: OrderBook tests
add_executable(test_main test/test_main.cpp)

# Phase 3: SPSC queue tests
add_executable(test_spsc test/test_spsc.cpp)
target_link_libraries(test_spsc PRIVATE pthread)

# Phase 3-4: FIX parser tests
add_executable(test_fix test/test_fix.cpp)

# Phase 4: Multi-symbol engine tests
add_executable(test_multibook test/test_multibook.cpp)

# ── Executables ───────────────────────────────────────────────────────────────
# Phase 3: Dual-thread engine demo
add_executable(engine src/engine.cpp)
target_link_libraries(engine PRIVATE pthread)

# Phase 4: TCP server
add_executable(server src/server.cpp)
target_link_libraries(server PRIVATE pthread)

# ── Benchmark ─────────────────────────────────────────────────────────────────
add_executable(bench bench/bench.cpp)
target_compile_options(bench PRIVATE -O3)
