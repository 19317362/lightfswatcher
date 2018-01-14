cmake_minimum_required(VERSION 3.9)

project(lightfswatcher)

set(CMAKE_CXX_STANDARD 11)
set(SOURCE_FILES test.cpp watch.h)

link_libraries(pthread)

add_executable(lightfswatcher_test ${SOURCE_FILES})


