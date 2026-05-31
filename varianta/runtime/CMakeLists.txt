# Variant A host-runtime scaffold (TASK 4). Compiles the generated ppc/ against a host runtime.
# NOTE: ../ppc/ is git-ignored — regenerate it first (see varianta/README.md "Reproduce"),
# then run gen_import_stubs.py to emit import_stubs.gen.cpp, then configure+build this.
cmake_minimum_required(VERSION 3.20)
project(sp_td_varianta CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# rexglue-recomps/third_party/XenonRecomp (provides ppc_context.h + simde)
set(XR "${CMAKE_CURRENT_SOURCE_DIR}/../../../third_party/XenonRecomp")
set(PPC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../ppc")

file(GLOB PPC_SOURCES "${PPC_DIR}/ppc_recomp.*.cpp" "${PPC_DIR}/ppc_func_mapping.cpp")
file(GLOB RT_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")

if(NOT PPC_SOURCES)
    message(FATAL_ERROR "No generated ppc/ sources found — regenerate ppc/ first (see varianta/README.md).")
endif()

add_executable(sp_td_varianta ${PPC_SOURCES} ${RT_SOURCES})
target_include_directories(sp_td_varianta PRIVATE
    "${PPC_DIR}"
    "${XR}/XenonUtils"
    "${XR}/thirdparty/simde")
# -O1: the recompiled TUs are huge; -w silences guest-code warnings (e.g. unused, signedness).
target_compile_options(sp_td_varianta PRIVATE -O1 -w -fno-strict-aliasing)
