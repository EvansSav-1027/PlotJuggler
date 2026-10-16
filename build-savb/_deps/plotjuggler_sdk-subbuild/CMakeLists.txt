# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file Copyright.txt or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION 3.28.3)

# Reject any attempt to use a toolchain file. We must not use one because
# we could be downloading it here. If the CMAKE_TOOLCHAIN_FILE environment
# variable is set, the cache variable will have been initialized from it.
unset(CMAKE_TOOLCHAIN_FILE CACHE)
unset(ENV{CMAKE_TOOLCHAIN_FILE})

# We name the project and the target for the ExternalProject_Add() call
# to something that will highlight to the user what we are working on if
# something goes wrong and an error message is produced.

project(plotjuggler_sdk-populate NONE)



include(ExternalProject)
ExternalProject_Add(plotjuggler_sdk-populate
                     "UPDATE_DISCONNECTED" "False" "DOWNLOAD_EXTRACT_TIMESTAMP" "FALSE" "EXTERNALPROJECT_INTERNAL_ARGUMENT_SEPARATOR" "URL" "https://github.com/PlotJuggler/plotjuggler_sdk/archive/refs/tags/v0.34.1.tar.gz" "URL_HASH" "SHA256=b42b3d53f13cd495c3e8a73e5728503b1cc6949c9dd820e164ba449f22349c32"
                    SOURCE_DIR          "/home/sav-sys-1027/PlotJuggler/build-savb/_deps/plotjuggler_sdk-src"
                    BINARY_DIR          "/home/sav-sys-1027/PlotJuggler/build-savb/_deps/plotjuggler_sdk-build"
                    CONFIGURE_COMMAND   ""
                    BUILD_COMMAND       ""
                    INSTALL_COMMAND     ""
                    TEST_COMMAND        ""
                    USES_TERMINAL_DOWNLOAD  YES
                    USES_TERMINAL_UPDATE    YES
                    USES_TERMINAL_PATCH     YES
)


