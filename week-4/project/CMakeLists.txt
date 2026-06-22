# ===============================================================
#  CSoT'26 Low Latency — Week 4 Project (lock-free feed pipeline)
#  Starter CMakeLists.txt. Extends the Week-3 template; same threads.
#
#  Build (Release, the default) with the shipped stub:
#      cmake -B build && cmake --build build -j
#      ./build/pipeline_runner data/tiny.feed
#
#  Build YOUR pipeline instead of the stub:
#      cmake -B build -DCSOT_PIPE_SRC=pipeline.cpp
#      cmake --build build -j
#
#  Build the way the judge does (portable baseline, no -march=native):
#      cmake -B build-judge -DCSOT_JUDGE_BUILD=ON -DCSOT_PIPE_SRC=pipeline.cpp
#      cmake --build build-judge -j
#
#  Build with ThreadSanitizer (find races in your ring buffer — see 03-/04-):
#      cmake -B build-tsan -DCMAKE_BUILD_TYPE=Debug -DENABLE_TSAN=ON -DCSOT_PIPE_SRC=pipeline.cpp
#      cmake --build build-tsan -j
#
#  Build with AddressSanitizer + UBSan (NOT for benchmarks; incompatible w/ TSan):
#      cmake -B build-asan -DCMAKE_BUILD_TYPE=Debug -DENABLE_SANITIZERS=ON
#      cmake --build build-asan -j
# ===============================================================

cmake_minimum_required(VERSION 3.20)
project(csot_pipeline CXX)

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

# Which translation unit holds your pipeline. Defaults to the shipped stub so a
# fresh checkout builds; point it at your own file once you start writing.
set(CSOT_PIPE_SRC "samples/pipeline.stub.cpp"
    CACHE STRING "Path to the single .cpp implementing create_pipeline()")

# CSOT_JUDGE_BUILD mirrors the judge toolchain: a portable ISA baseline instead
# of -march=native, so a binary built here behaves like the one the judge
# builds from your upload. Turn it ON before trusting your numbers as
# "judge-like"; leave it OFF for the fastest local builds on your own CPU.
option(CSOT_JUDGE_BUILD "Build with the judge's portable flags (-march=x86-64-v2)" OFF)

# -------- Threads --------
# The pipeline runs a producer and a consumer thread that you spawn and pin
# inside run(). We link the system threading library and pass -pthread, exactly
# as the judge does.
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

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

# -------- Optional: ThreadSanitizer (data-race detector) --------
# Mutually exclusive with AddressSanitizer; use a separate build dir.
option(ENABLE_TSAN "Build with ThreadSanitizer" OFF)
if(ENABLE_TSAN)
    add_compile_options(-fsanitize=thread -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=thread)
endif()

# -------- Optional: AddressSanitizer + UBSan --------
option(ENABLE_SANITIZERS "Build with AddressSanitizer + UBSan" OFF)
if(ENABLE_SANITIZERS AND ENABLE_TSAN)
    message(FATAL_ERROR "ENABLE_SANITIZERS and ENABLE_TSAN are mutually exclusive; pick one build dir each.")
endif()
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
# Links the shipped harness main() against your pipeline translation unit.
# CLI: ./pipeline_runner <feed_file>
add_executable(pipeline_runner harness/main.cpp ${CSOT_PIPE_SRC})
target_link_libraries(pipeline_runner PRIVATE Threads::Threads)

# ===============================================================
# Help message after first configure
message(STATUS "")
message(STATUS "================================================================")
message(STATUS "  Build type:        ${CMAKE_BUILD_TYPE}")
message(STATUS "  Pipeline source:   ${CSOT_PIPE_SRC}")
message(STATUS "  Judge build:       ${CSOT_JUDGE_BUILD}")
message(STATUS "  ThreadSanitizer:   ${ENABLE_TSAN}")
message(STATUS "  ASan/UBSan:        ${ENABLE_SANITIZERS}")
message(STATUS "  LTO (Release):     ${ENABLE_LTO}")
message(STATUS "  Compiler:          ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "================================================================")
message(STATUS "")
