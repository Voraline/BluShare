cmake_minimum_required(VERSION 3.20)
project(BluShareServer CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
if(MSVC)
    add_compile_options(/O2 /Oi /GL /W3 /EHsc /DNDEBUG /arch:AVX2 /Ot)
    add_link_options(/LTCG)
endif()

include(FetchContent)
set(OPUS_BUILD_SHARED_LIBRARY OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    opus
    GIT_REPOSITORY https://github.com/xiph/opus.git
    GIT_TAG v1.6.1
)
FetchContent_MakeAvailable(opus)

add_executable(BluShareServer
    Src/Main.cpp
    Src/AudioCapture.cpp
    Src/BluetoothServer.cpp
)
target_include_directories(BluShareServer PRIVATE Src)
target_link_libraries(BluShareServer PRIVATE Ws2_32 Bthprops Ole32 Avrt opus)
