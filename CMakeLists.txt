cmake_minimum_required(VERSION 3.20)

project(quill_match
    VERSION 0.1.0
    DESCRIPTION "Low-latency limit order book and matching engine"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(QUILL_BUILD_TESTS "Build the unit tests" ON)
option(QUILL_BUILD_BENCHMARKS "Build the Google Benchmark suite" ON)
option(QUILL_BUILD_APPS "Build the quill command line tool" ON)
option(QUILL_WERROR "Treat compiler warnings as errors" ON)
option(QUILL_SANITIZE "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(QUILL_NATIVE "Tune code generation for the build machine (-march=native)" OFF)

include(cmake/CompilerOptions.cmake)

add_subdirectory(src)

if(QUILL_BUILD_APPS)
    add_subdirectory(apps)
endif()

if(QUILL_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(QUILL_BUILD_BENCHMARKS)
    add_subdirectory(bench)
endif()
