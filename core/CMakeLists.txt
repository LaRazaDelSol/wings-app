# Wings core - host-compilable Pegasus DL model + adapter. SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.16)
project(wings_core CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(wings_core STATIC
  src/json.cpp
  src/http.cpp
  src/logic.cpp
  src/pegasus_v1.cpp
  src/connect.cpp)
target_include_directories(wings_core PUBLIC include)
if(WIN32)
  target_link_libraries(wings_core PUBLIC ws2_32)
endif()
if(NOT MSVC)
  target_compile_options(wings_core PRIVATE -Wall -Wextra)
endif()

option(WINGS_CORE_TESTS "Build the offline tests" ON)
if(WINGS_CORE_TESTS)
  enable_testing()
  add_executable(wings_core_tests
    tests/test_main.cpp tests/test_logic.cpp tests/test_samples.cpp
    tests/test_writes.cpp tests/test_connect.cpp tests/test_wire.cpp tests/test_stream.cpp)
  target_include_directories(wings_core_tests PRIVATE tests)
  target_link_libraries(wings_core_tests PRIVATE wings_core)
  # Tests read recon/samples and tests/fixtures relative to wings/core.
  add_test(NAME wings_core_tests COMMAND wings_core_tests WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
endif()

option(WINGS_CORE_SMOKE "Build the read-only live smoke tool" ON)
if(WINGS_CORE_SMOKE)
  add_executable(wings_smoke tools/smoke.cpp)
  target_link_libraries(wings_smoke PRIVATE wings_core)
endif()
