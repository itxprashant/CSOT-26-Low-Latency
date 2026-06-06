# ===============================================================
#  CSoT'26 Low Latency — Week 2 Project (cache simulator)
#  Starter CMakeLists.txt. Extends the Week-1 template.
#
#  Build (Release, the default) with the shipped stub:
#      cmake -B build && cmake --build build -j
#      ./build/cache_sim_runner data/tiny.trace
#
#  Build YOUR simulator instead of the stub:
#      cmake -B build -DCSOT_CACHE_SIM_SRC=cache_sim.cpp
#      cmake --build build -j
#
#  Build the way the judge does (portable baseline, no -march=native):
#      cmake -B build-judge -DCSOT_JUDGE_BUILD=ON -DCSOT_CACHE_SIM_SRC=cache_sim.cpp
#      cmake --build build-judge -j
#
#  Build with sanitizers (debug runs, NOT for benchmarks):
#      cmake -B build-asan -DCMAKE_BUILD_TYPE=Debug -DENABLE_SANITIZERS=ON
#      cmake --build build-asan -j
# ===============================================================

cmake_minimum_required(VERSION 3.20)
project(csot_cache_sim CXX)

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

# Which translation unit holds your simulator. Defaults to the shipped stub so
# a fresh checkout builds; point it at your own file once you start writing.
set(CSOT_CACHE_SIM_SRC "samples/cache_sim.stub.cpp"
    CACHE STRING "Path to the single .cpp implementing create_cache_sim()")

# CSOT_JUDGE_BUILD mirrors the judge toolchain: a portable ISA baseline instead
# of -march=native, so a binary built here behaves like the one the judge
# builds from your upload. Turn it ON before trusting your numbers as
# "judge-like"; leave it OFF for the fastest local builds on your own CPU.
option(CSOT_JUDGE_BUILD "Build with the judge's portable flags (-march=x86-64-v2)" OFF)

# -------- Warnings --------
add_compile_options(
    -Wall -Wextra -Wpedantic
    -Wshadow -Wnon-virtual-dtor
    -Wcast-align -Wunused
    -Woverloaded-virtual -Wnull-dereference
)

# -------- Tuning (Release only) --------
add_compile_options($<$<CONFIG:Release>:-O3>)
add_compile_options($<$<CONFIG:Release>:-fno-omit-frame-pointer>)   # better perf/flame-graph stacks
if(CSOT_JUDGE_BUILD)
    add_compile_options($<$<CONFIG:Release>:-march=x86-64-v2>)
else()
    add_compile_options($<$<CONFIG:Release>:-march=native>)
endif()

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

# -------- Local runner --------
# Links the shipped harness main() against your simulator translation unit.
# CLI: ./cache_sim_runner <trace_file>
add_executable(cache_sim_runner harness/main.cpp ${CSOT_CACHE_SIM_SRC})

# ===============================================================
# Help message after first configure
message(STATUS "")
message(STATUS "================================================================")
message(STATUS "  Build type:        ${CMAKE_BUILD_TYPE}")
message(STATUS "  Simulator source:  ${CSOT_CACHE_SIM_SRC}")
message(STATUS "  Judge build:       ${CSOT_JUDGE_BUILD}")
message(STATUS "  Sanitizers:        ${ENABLE_SANITIZERS}")
message(STATUS "  LTO (Release):     ${ENABLE_LTO}")
message(STATUS "  Compiler:          ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "================================================================")
message(STATUS "")
