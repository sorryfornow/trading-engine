cmake_minimum_required(VERSION 3.16)
project(trading-engine)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O2 -Wall -Wextra")

include_directories(include)

add_executable(trading-engine src/main.cpp
        include/MatchingEngine.h)
