# SPDX-License-Identifier: GPL-3.0-or-later

# GoogleTest
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)

add_subdirectory(googletest EXCLUDE_FROM_ALL)

# libsbc for the host tests: the plain C encoder only, on every CPU
# (src/compat/config.h switches the SIMD paths off). The console build
# in the Makefile adds the MMX and SSE files.
add_library(sbc_host STATIC
  sbc/sbc/sbc.c
  sbc/sbc/sbc_primitives.c
)

target_include_directories(sbc_host PUBLIC
  "${PROJECT_SOURCE_DIR}/src/compat"
  "${CMAKE_CURRENT_SOURCE_DIR}/sbc/sbc"
)

target_compile_definitions(sbc_host PUBLIC
  HAVE_CONFIG_H
  BLUEV_HOST_BUILD
)

target_compile_options(sbc_host PRIVATE
  -O2
  -w
  -include "${PROJECT_SOURCE_DIR}/src/compat/config.h"
)
