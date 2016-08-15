cmake_minimum_required (VERSION 3.5)
project (memchecker)

set(CMAKE_CXXFLAGS "${CMAKE_CXXFLAGS} -O0 -g")

add_library(memchecker SHARED checker.cpp)
target_link_libraries(memchecker pthread)


add_executable(memchecker_test main.cpp)
target_link_libraries(memchecker_test memchecker)
