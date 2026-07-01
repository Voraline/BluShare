cmake_minimum_required(VERSION 3.22)
project(BluetoothAudioNative CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(BluetoothAudioNative SHARED NativeBridge.cpp)

find_library(LogLib log)
find_library(AAudioLib aaudio)

target_link_libraries(BluetoothAudioNative ${LogLib} ${AAudioLib})
target_compile_options(BluetoothAudioNative PRIVATE -O3)
