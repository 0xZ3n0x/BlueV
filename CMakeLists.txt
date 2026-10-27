# SPDX-License-Identifier: GPL-3.0-or-later
# Host build: unit, protocol and service tests (gtest/gmock). The console
# image is built with the Makefile; this file never runs on the console.
#
#   cmake -S . -B build-host -G Ninja [-DBLUEV_SANITIZE=thread|address]
#   cmake --build build-host && ctest --test-dir build-host --output-on-failure
cmake_minimum_required(VERSION 3.20)

# the version of the program, kept in one file (the Makefile reads it too)
file(STRINGS VERSION BLUEV_VERSION LIMIT_COUNT 1)
project(bluev_host VERSION ${BLUEV_VERSION} LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(BLUEV_SANITIZE "" CACHE STRING "thread, address or empty")
if(BLUEV_SANITIZE)
  add_compile_options(-fsanitize=${BLUEV_SANITIZE} -fno-omit-frame-pointer -g)
  add_link_options(-fsanitize=${BLUEV_SANITIZE})
endif()
add_compile_options(-Wall -Wextra -Wno-unused-parameter)

# ---- third party -----------------------------------------------------------
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
add_subdirectory(third_party/googletest EXCLUDE_FROM_ALL)

# libsbc, unmodified, with the same SIMD paths as the console build
add_library(sbc_host STATIC
  third_party/sbc/sbc/sbc.c
  third_party/sbc/sbc/sbc_primitives.c
  third_party/sbc/sbc/sbc_primitives_mmx.c
  third_party/sbc/sbc/sbc_primitives_sse.c)
target_include_directories(sbc_host PUBLIC
  ${CMAKE_SOURCE_DIR}/src/compat third_party/sbc/sbc)
target_compile_definitions(sbc_host PUBLIC HAVE_CONFIG_H BLUEV_HOST_BUILD)
target_compile_options(sbc_host PRIVATE -O2 -mmmx -msse4.2 -w -include ${CMAKE_SOURCE_DIR}/src/compat/config.h)

# ---- the code under test ---------------------------------------------------
file(GLOB_RECURSE CORE_SOURCES CONFIGURE_DEPENDS src/core/*.cpp)
file(GLOB_RECURSE POSIX_SOURCES CONFIGURE_DEPENDS src/platform/posix/*.cpp)
add_library(bluev_core STATIC ${CORE_SOURCES} ${POSIX_SOURCES})
target_include_directories(bluev_core PUBLIC src)
target_compile_options(bluev_core PRIVATE -fno-exceptions -fno-rtti)
target_link_libraries(bluev_core PUBLIC sbc_host)

file(GLOB_RECURSE SERVICE_SOURCES CONFIGURE_DEPENDS src/service/*.cpp)
add_library(bluev_service STATIC ${SERVICE_SOURCES})
target_compile_options(bluev_service PRIVATE -fno-exceptions -fno-rtti)
target_link_libraries(bluev_service PUBLIC bluev_core)

# ---- tests -------------------------------------------------------------------
enable_testing()
include(GoogleTest)
file(GLOB_RECURSE TEST_SOURCES CONFIGURE_DEPENDS
  tests/unit/*.cpp tests/protocol/*.cpp tests/service/*.cpp tests/support/*.cpp)
add_executable(bluev_tests ${TEST_SOURCES})
target_include_directories(bluev_tests PRIVATE tests/support)
target_link_libraries(bluev_tests PRIVATE bluev_service bluev_core GTest::gtest_main GTest::gmock pthread)
gtest_discover_tests(bluev_tests DISCOVERY_TIMEOUT 60)
