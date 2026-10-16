# SPDX-License-Identifier: MPL-2.0
#
# SAV log loader plugin — a standalone project, not part of the PJ4 app build
# (PJ4 loads plugins at runtime). See README.md.
#
#   cmake -S plugins/data_load_savb -B build-savb -DCMAKE_BUILD_TYPE=Release
#   cmake --build build-savb -j
#   ctest --test-dir build-savb --output-on-failure
#   ./run.sh --plugin-dir build-savb/bin
cmake_minimum_required(VERSION 3.22)
project(pj_data_load_savb LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(SAVB_BUILD_TESTS "Build the plugin tests" ON)

include(FetchContent)
if(POLICY CMP0135)
  cmake_policy(SET CMP0135 NEW)
endif()

# Dependencies of the SDK (and of this plugin): use an installed package when
# there is one, otherwise fetch it. FIND_PACKAGE_ARGS makes FetchContent try
# find_package() first.
FetchContent_Declare(nlohmann_json
  URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
  FIND_PACKAGE_ARGS 3.11)
FetchContent_Declare(fmt
  URL https://github.com/fmtlib/fmt/archive/refs/tags/12.0.0.tar.gz
  FIND_PACKAGE_ARGS)
FetchContent_Declare(FastFloat
  URL https://github.com/fastfloat/fast_float/archive/refs/tags/v8.0.2.tar.gz
  FIND_PACKAGE_ARGS)
set(_savb_deps nlohmann_json fmt FastFloat)
if(SAVB_BUILD_TESTS)
  FetchContent_Declare(GTest
    URL https://github.com/google/googletest/archive/refs/tags/v1.17.0.tar.gz
    FIND_PACKAGE_ARGS)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  list(APPEND _savb_deps GTest)
endif()
FetchContent_MakeAvailable(${_savb_deps})
if(NOT TARGET FastFloat::fast_float AND TARGET fast_float)
  add_library(FastFloat::fast_float ALIAS fast_float)
endif()
if(NOT TARGET GTest::gtest_main AND TARGET gtest_main)
  add_library(GTest::gtest_main ALIAS gtest_main)
endif()

# The plugin SDK, at the version the app pins (conanfile.txt / root CMakeLists.txt).
# An installed SDK (e.g. from the app's Conan build tree on CMAKE_PREFIX_PATH)
# is used when found; otherwise the pinned release is built from source.
find_package(plotjuggler_sdk CONFIG QUIET COMPONENTS plugin_sdk plugin_host)
if(NOT plotjuggler_sdk_FOUND)
  set(PJ_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(PJ_BUILD_PORTED_PLUGINS OFF CACHE BOOL "" FORCE)
  # The fetched fast_float headers are not -Wold-style-cast clean; the SDK is
  # consumed as-is, so only this plugin's own code is built with -Werror.
  set(PJ_WARNINGS_AS_ERRORS OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(plotjuggler_sdk
    URL https://github.com/PlotJuggler/plotjuggler_sdk/archive/refs/tags/v0.34.1.tar.gz
    URL_HASH SHA256=b42b3d53f13cd495c3e8a73e5728503b1cc6949c9dd820e164ba449f22349c32)
  FetchContent_MakeAvailable(plotjuggler_sdk)
endif()

if(MSVC)
  set(SAVB_WARNING_FLAGS /W4 /WX /permissive-)
else()
  set(SAVB_WARNING_FLAGS -Wall -Wextra -Werror -Wshadow -Wconversion -Wold-style-cast -Wpedantic)
endif()

# Format reader, shared by the plugin and the tests.
add_library(savb_log STATIC savb_log.cpp)
target_include_directories(savb_log PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(savb_log PRIVATE ${SAVB_WARNING_FLAGS})

set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/bin)
add_library(savb_source_plugin SHARED savb_source.cpp)
target_compile_options(savb_source_plugin PRIVATE ${SAVB_WARNING_FLAGS})
target_link_libraries(savb_source_plugin PRIVATE plotjuggler_sdk::plugin_sdk savb_log nlohmann_json::nlohmann_json)
pj_configure_plugin(savb_source_plugin
  FAMILIES        data_source
  MANIFEST_FILE   ${CMAKE_CURRENT_SOURCE_DIR}/manifest.json
  MANIFEST_HEADER generated/savb_manifest.hpp
  MANIFEST_VAR    kSavbManifest)

if(SAVB_BUILD_TESTS)
  enable_testing()
  add_executable(savb_source_test tests/savb_source_test.cpp)
  target_compile_options(savb_source_test PRIVATE ${SAVB_WARNING_FLAGS})
  target_compile_definitions(savb_source_test PRIVATE
    SAVB_PLUGIN_PATH="$<TARGET_FILE:savb_source_plugin>")
  target_link_libraries(savb_source_test PRIVATE plotjuggler_sdk::plugin_host savb_log GTest::gtest_main)
  add_dependencies(savb_source_test savb_source_plugin)
  add_test(NAME savb_source_test COMMAND savb_source_test)
endif()
