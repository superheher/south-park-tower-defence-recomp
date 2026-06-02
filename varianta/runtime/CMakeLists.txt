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
# -O0 for bring-up (the recompiled TUs are huge — keeps compile fast + RAM low); raise later.
# -w silences guest-code warnings (unused, signedness, etc.).
target_compile_options(sp_td_varianta PRIVATE -O0 -w -fno-strict-aliasing
    -include "${CMAKE_CURRENT_SOURCE_DIR}/rex_indirect.h")   # log+skip null indirect-call targets

# Link the prebuilt XenonUtils (XEX parse/decrypt/decompress; AES+LZX compiled in) + its deps.
# These come from the XenonRecomp build tree; (re)build them with: cmake --build ${XR}/out/build.
set(XR_BUILD "${XR}/out/build")
find_package(Threads REQUIRED)   # std::thread (ExCreateThread / vblank pump)

# Minimal from-scratch Vulkan renderer (vulkan_render.cpp): host window + present. Active only at
# runtime under REX_RENDER=1, but always linked.
find_package(Vulkan REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2 REQUIRED sdl2)
target_include_directories(sp_td_varianta PRIVATE ${SDL2_INCLUDE_DIRS})

target_link_libraries(sp_td_varianta PRIVATE
    "${XR_BUILD}/XenonUtils/libXenonUtils.a"
    "${XR_BUILD}/thirdparty/disasm/libdisasm.a"
    "${XR_BUILD}/thirdparty/fmt/libfmt.a"
    Threads::Threads
    Vulkan::Vulkan
    ${SDL2_LIBRARIES})
