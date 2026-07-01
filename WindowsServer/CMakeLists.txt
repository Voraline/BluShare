cmake_minimum_required(VERSION 3.20)
project(BluetoothAudioServer CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(MSVC)
    add_compile_options(/O2 /W3 /EHsc)
endif()
add_executable(BluetoothAudioServer
    Src/Main.cpp
    Src/AudioCapture.cpp
    Src/BluetoothServer.cpp
)
target_include_directories(BluetoothAudioServer PRIVATE Src)
target_link_libraries(BluetoothAudioServer PRIVATE Ws2_32 Bthprops Ole32 Avrt)
