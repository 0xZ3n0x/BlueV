# SPDX-License-Identifier: GPL-3.0-or-later

include(GoogleTest)

file(GLOB_RECURSE TEST_SOURCES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/unit/*.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/protocol/*.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/service/*.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/support/*.cpp"
)

add_executable(bluev_tests ${TEST_SOURCES})

target_include_directories(bluev_tests PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/support"
)

target_link_libraries(bluev_tests PRIVATE
  bluev_service
  bluev_core
  GTest::gtest_main
  GTest::gmock
  pthread
)

gtest_discover_tests(bluev_tests DISCOVERY_TIMEOUT 60)
