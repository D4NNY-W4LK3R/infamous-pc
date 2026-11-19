# SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later

# Stand-alone build of the adaptive optimizer and its tests. The optimizer core depends only on
# the C++20 standard library, so it is built and tested without the emulator's dependencies:
#   cmake -S tests/adaptive -B build-adaptive -DGTEST_SOURCE_DIR=<googletest checkout>
#   cmake --build build-adaptive && build-adaptive/adaptive_tests

cmake_minimum_required(VERSION 3.20)
project(adaptive_tests CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(GTEST_SOURCE_DIR "" CACHE PATH "googletest source tree (v1.17.0)")
option(ADAPTIVE_SANITIZE "Build with address and undefined-behavior sanitizers" OFF)

if (GTEST_SOURCE_DIR)
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    add_subdirectory(${GTEST_SOURCE_DIR} ${CMAKE_BINARY_DIR}/googletest EXCLUDE_FROM_ALL)
else()
    include(FetchContent)
    FetchContent_Declare(googletest
        URL https://github.com/google/googletest/archive/refs/tags/v1.17.0.zip)
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
endif()

# googletest 1.17 builds itself with warnings as errors; Clang 22 (Visual Studio 18's clang-cl)
# added -Wcharacter-conversion, which fires inside googletest's own headers.
if (CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    foreach(t gtest gtest_main)
        if (TARGET ${t})
            target_compile_options(${t} PRIVATE -Wno-unknown-warning-option -Wno-character-conversion)
        endif()
    endforeach()
endif()

set(SRC ${CMAKE_CURRENT_SOURCE_DIR}/../../src)
set(ADAPTIVE_SOURCES
    ${SRC}/adaptive/brain.cpp
    ${SRC}/adaptive/contracts.cpp
    ${SRC}/adaptive/hardware.cpp
    ${SRC}/adaptive/knobs.cpp
    ${SRC}/adaptive/knowledge.cpp
    ${SRC}/adaptive/mini_brains.cpp
    ${SRC}/adaptive/safety.cpp
    ${SRC}/adaptive/settings.cpp
    ${SRC}/adaptive/stats.cpp
    ${SRC}/adaptive/telemetry.cpp
    ${SRC}/input/camera_hook_signature.cpp
    ${SRC}/core/second_son_patches.cpp
    ${SRC}/common/shared_user_dir.cpp
)

add_library(adaptive_core STATIC ${ADAPTIVE_SOURCES})
# The game patches' planning is pure; Apply (logging, VirtualQuery) belongs to the emulator.
target_compile_definitions(adaptive_core PUBLIC SECOND_SON_PATCHES_STANDALONE)
target_include_directories(adaptive_core PUBLIC ${SRC})
if (MSVC)
    target_compile_options(adaptive_core PRIVATE /W4)
else()
    target_compile_options(adaptive_core PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

add_executable(adaptive_tests
    test_stats.cpp
    test_telemetry.cpp
    test_knowledge.cpp
    test_safety.cpp
    test_brain.cpp
    test_mini_brains.cpp
    test_settings.cpp
    test_second_son_patches.cpp
    test_shared_user_dir.cpp
    test_camera_signature.cpp
)
target_link_libraries(adaptive_tests PRIVATE adaptive_core GTest::gtest_main)

if (ADAPTIVE_SANITIZE AND NOT MSVC)
    foreach(t adaptive_core adaptive_tests)
        target_compile_options(${t} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(${t} PRIVATE -fsanitize=address,undefined)
    endforeach()
endif()

enable_testing()
add_test(NAME adaptive_tests COMMAND adaptive_tests)
