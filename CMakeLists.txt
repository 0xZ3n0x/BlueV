# SPDX-License-Identifier: GPL-3.0-or-later
# Host build: unit, protocol and service tests.
# Console image is built separately with the Makefile.

cmake_minimum_required(VERSION 3.20)

file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" BLUEV_VERSION LIMIT_COUNT 1)
project(bluev_host VERSION ${BLUEV_VERSION} LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(BLUEV_SANITIZE "" CACHE STRING "thread, address or empty")

if(BLUEV_SANITIZE)
  if(NOT BLUEV_SANITIZE MATCHES "^(thread|address)$")
    message(FATAL_ERROR "BLUEV_SANITIZE must be empty, thread or address")
  endif()
  add_compile_options(-fsanitize=${BLUEV_SANITIZE} -fno-omit-frame-pointer -g)
  add_link_options(-fsanitize=${BLUEV_SANITIZE})
endif()

add_compile_options(-Wall -Wextra -Wno-unused-parameter)

add_subdirectory(third_party)
add_subdirectory(src)

include(CTest)

if(BUILD_TESTING)
  add_subdirectory(tests)
endif()
