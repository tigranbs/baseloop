cmake_minimum_required(VERSION 3.6)
project(baseloop)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++11")

include_directories(./src)

set(SOURCE_FILES
        main.cpp
        src/baseloop.h)

add_executable(baseloop main.cpp ${SOURCE_FILES})
