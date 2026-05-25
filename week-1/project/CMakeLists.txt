# ===============================================================
#  CSoT'26 Low Latency — Week 1 Project
#  Starter CMakeLists.txt. Customize as needed.
#
#  Build (Release, the default):
#      cmake -B build && cmake --build build -j
#
#  Build with sanitizers (debug runs, NOT for benchmarks):
#      cmake -B build-asan -DCMAKE_BUILD_TYPE=Debug -DENABLE_SANITIZERS=ON
#      cmake --build build-asan -j
#
#  Run:
#      ./build/quant_runner path/to/your_strategy.so data/synthetic_small.csv
#      ./build/quant_bench
# ===============================================================

cmake_minimum_required(VERSION 3.20)
project(csot_quant_platform CXX)

set(CMAKE_CXX_STANDARD          20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS        OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)   # for clangd / IDE tooling

# Default to Release if the user didn't specify.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
                 Debug Release RelWithDebInfo MinSizeRel)
endif()

# -------- Warnings --------
add_compile_options(
    -Wall -Wextra -Wpedantic
    -Wshadow -Wnon-virtual-dtor
    -Wcast-align -Wunused
    -Woverloaded-virtual -Wnull-dereference
)

# -------- Tuning (Release only) --------
add_compile_options($<$<CONFIG:Release>:-O2>)
add_compile_options($<$<CONFIG:Release>:-march=native>)
add_compile_options($<$<CONFIG:Release>:-fno-omit-frame-pointer>)   # better perf/flame-graph stacks

# -------- Optional: sanitizers --------
option(ENABLE_SANITIZERS "Build with AddressSanitizer + UBSan" OFF)
if(ENABLE_SANITIZERS)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=address,undefined)
endif()

# -------- Optional: LTO for Release --------
option(ENABLE_LTO "Enable link-time optimization for Release" ON)
if(ENABLE_LTO AND CMAKE_BUILD_TYPE STREQUAL "Release")
    include(CheckIPOSupported)
    check_ipo_supported(RESULT lto_ok OUTPUT lto_err)
    if(lto_ok)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)
    else()
        message(WARNING "LTO not supported on this toolchain: ${lto_err}")
    endif()
endif()

# -------- Include path --------
include_directories(${CMAKE_SOURCE_DIR}/include)

# -------- Engine library --------
# Uncomment when you've written src/engine.cpp.
# add_library(engine STATIC src/engine.cpp)
# target_link_libraries(engine PUBLIC ${CMAKE_DL_LIBS})   # dlopen / dlsym

# -------- Main runner --------
# CLI: ./quant_runner <strategy.so> <ticks.csv>
# Uncomment when you've written src/main.cpp.
# add_executable(quant_runner src/main.cpp)
# target_link_libraries(quant_runner PRIVATE engine)

# -------- A sample strategy as a shared library --------
# The engine will dlopen this and call create_strategy().
# Uncomment when you've written strategies/spec_strategy.cpp.
# add_library(spec_strategy SHARED strategies/spec_strategy.cpp)
# set_target_properties(spec_strategy PROPERTIES PREFIX "")   # produce "spec_strategy.so" not "libspec_strategy.so"

# -------- Benchmarks (Google Benchmark) --------
# Uncomment when you've written bench/bench.cpp.
# find_package(benchmark REQUIRED)
# add_executable(quant_bench bench/bench.cpp)
# target_link_libraries(quant_bench PRIVATE engine benchmark::benchmark)

# ===============================================================
# Help message after first configure
message(STATUS "")
message(STATUS "================================================================")
message(STATUS "  Build type:        ${CMAKE_BUILD_TYPE}")
message(STATUS "  Sanitizers:        ${ENABLE_SANITIZERS}")
message(STATUS "  LTO (Release):     ${ENABLE_LTO}")
message(STATUS "  Compiler:          ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "================================================================")
message(STATUS "")
