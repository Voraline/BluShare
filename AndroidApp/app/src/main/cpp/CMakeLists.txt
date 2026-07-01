cmake_minimum_required(VERSION 3.22)
project(BluShareNative CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(BluShareNative SHARED
    NativeBridge.cpp
)

find_library(LogLib    log)
find_library(AAudioLib aaudio)

target_link_libraries(BluShareNative
    ${LogLib}
    ${AAudioLib}
)

target_compile_options(BluShareNative PRIVATE
    -O3
    -march=armv8.2-a+simd
    -mtune=cortex-a55
    -flto=thin
    -funroll-loops
    -fomit-frame-pointer
    -fno-plt
    -fno-semantic-interposition
    -fno-common
    -fno-math-errno
    -ffp-contract=on
    -fno-stack-protector
    -fno-unwind-tables
    -fno-asynchronous-unwind-tables
    -fno-exceptions
    -ffunction-sections
    -fdata-sections
    -fvisibility=hidden
    -DNDEBUG
    -U_FORTIFY_SOURCE
    -D_FORTIFY_SOURCE=0
)
target_link_options(BluShareNative PRIVATE
    -flto=thin
    -Wl,--gc-sections
    -Wl,-O3
    -Wl,--as-needed
    -Wl,--icf=all
    -Wl,-z,norelro
    -Wl,-z,noseparate-code
    -Wl,--build-id=none
)
