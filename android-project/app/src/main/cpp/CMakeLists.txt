#
# @license
# SPDX-License-Identifier: Apache-2.0
#
# CMakeLists.txt for Qualcomm Snapdragon FM Radio Native Bridge
# Supports Android 16 (API 36) and NDK r27 builds.
#

cmake_minimum_required(VERSION 3.22.1)

project("fm_jni" LANGUAGES CXX)

# Enforce modern C++20 for high-performance platform operations
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Add optimization and hardening flags for production builds
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O3 -fstack-protector-strong -Wall -Werror -Wformat -Wformat-security -D_FORTIFY_SOURCE=2")

# Define our shared native JNI library target
add_library(fm_jni SHARED
        fm_jni.cpp
)

# Search for system libraries inside the Android NDK sysroot
find_library(log-lib
        log
        REQUIRED
)

find_library(android-lib
        android
        REQUIRED
)

# Link our bridge against JNI logs, Android binder/window services, and dlopen dynamic loader
target_link_libraries(fm_jni
        ${log-lib}
        ${android-lib}
        dl
)

# Set compile definitions for binary security and debug logs
target_compile_definitions(fm_jni PRIVATE
        ANDROID_NDK_BUILD
)
