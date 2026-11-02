# SPDX-License-Identifier: GPL-3.0-or-later

file(GLOB_RECURSE CORE_SOURCES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/core/*.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/platform/posix/*.cpp"
)

add_library(bluev_core STATIC ${CORE_SOURCES})

target_include_directories(bluev_core PUBLIC
  "${PROJECT_SOURCE_DIR}/src"
)

target_compile_options(bluev_core PRIVATE -fno-exceptions -fno-rtti)

target_link_libraries(bluev_core PUBLIC sbc_host)

file(GLOB_RECURSE SERVICE_SOURCES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/service/*.cpp"
)

add_library(bluev_service STATIC ${SERVICE_SOURCES})

target_compile_options(bluev_service PRIVATE -fno-exceptions -fno-rtti)

target_link_libraries(bluev_service PUBLIC bluev_core)
