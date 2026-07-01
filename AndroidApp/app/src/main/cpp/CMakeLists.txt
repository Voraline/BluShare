cmake_minimum_required(VERSION 3.22)
project(BluShareNative CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(BluShareNative SHARED NativeBridge.cpp)

find_library(LogLib log)
find_library(AAudioLib aaudio)

target_link_libraries(BluShareNative ${LogLib} ${AAudioLib})
target_compile_options(BluShareNative PRIVATE -O3 -DNDEBUG -ffast-math -funroll-loops)
