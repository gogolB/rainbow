# Copyright 2021 Google LLC
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     https://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

cmake_minimum_required (VERSION 3.2)
project (rainbow VERSION 20211117 LANGUAGES CXX)
include(CTest)

# Config header
configure_file (
	"${PROJECT_SOURCE_DIR}/config.h.in"
	"${PROJECT_BINARY_DIR}/config.h"
)
include_directories("${PROJECT_BINARY_DIR}")

set(CMAKE_CXX_FLAGS_DEBUG "-g -Wall -O0")
set(CMAKE_CXX_FLAGS_RELEASE "-Wall -O2")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)	# we want c++17 not gnu++17

add_executable(rainbow rainbow.cc)

add_library(two_color two_color.cc)
add_library(system_topology system_topology.cc)
add_library(spraypaint spraypaint.cc)

# Needs abseil
find_package(absl REQUIRED)
target_link_libraries(two_color absl::str_format)
target_link_libraries(spraypaint absl::flags absl::str_format absl::strings)
target_link_libraries(rainbow absl::base absl::failure_signal_handler absl::symbolize absl::flags absl::flags_parse absl::str_format absl::time)

target_link_libraries(spraypaint two_color)
target_link_libraries(spraypaint system_topology)
target_link_libraries(rainbow spraypaint)

if (BUILD_TESTING)
	add_executable(system_topology_test system_topology_test.cc)
	target_link_libraries(system_topology_test system_topology)
	add_test(NAME system_topology_test COMMAND system_topology_test)

	add_executable(spraypaint_self_test spraypaint_self_test.cc)
	target_link_libraries(spraypaint_self_test spraypaint)
	add_test(NAME spraypaint_self_test COMMAND spraypaint_self_test)

	find_package(GTest QUIET)
	if (GTest_FOUND)
		add_executable(rainbow_gtest
			spraypaint_test.cc
			two_color_test.cc
			system_topology_gtest.cc)
		target_link_libraries(rainbow_gtest
			GTest::gmock
			GTest::gtest_main
			spraypaint
			system_topology
			two_color)
		add_test(NAME rainbow_gtest COMMAND rainbow_gtest)
	else()
		message(STATUS "GTest not found; gtest-based unit tests will be skipped")
	endif()
endif()
