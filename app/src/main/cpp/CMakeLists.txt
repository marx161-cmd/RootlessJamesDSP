cmake_minimum_required(VERSION 3.22.1)

# NOTE: -Ofast (below, pre-existing) already implies -ffast-math semantics —
# the "skip fast-math" decision made when scoping the SVE2/NEON optimization
# work isn't actually reflected in the real build yet, since this flag
# predates that discussion. Flagged, not touched here — dialing it back to
# -O3 is a separate, deliberate decision, not bundled into this LTO change.
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -ffunction-sections -fdata-sections -Ofast -ftree-vectorize -DDEBUG -Wno-incompatible-function-pointer-types -Wno-incompatible-pointer-types -Wno-implicit-int -Wno-implicit-function-declaration -flto")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -ffunction-sections -fdata-sections -Ofast -ftree-vectorize -DDEBUG -flto")
set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -flto")

project(jamesdsp LANGUAGES C CXX)
project(jamesdsp-wrapper LANGUAGES CXX)

include(libjdspimptoolbox/CMakeLists.txt)
include(libjamesdsp-wrapper/CMakeLists.txt)

# LSP DSP units + AArch64 NEON primitives — compiled from source
file(GLOB LSP_DSP_SOURCES
    lsp-dsp-lib/src/main/dsp.cpp
    lsp-dsp-lib/src/main/aarch64/aarch64.cpp
    lsp-dsp-lib/src/main/aarch64/asimd.cpp
    lsp-dsp-lib/src/main/arm/arm.cpp
    lsp-dsp-lib/src/main/generic/generic.cpp
    lsp-dsp-units/src/main/misc/interpolation.cpp
    lsp-dsp-units/src/main/dynamics/Compressor.cpp
    lsp-dsp-units/src/main/dynamics/Gate.cpp
    lsp-dsp-units/src/main/dynamics/Limiter.cpp
    lsp-dsp-units/src/main/dynamics/DynamicProcessor.cpp
    lsp-dsp-units/src/main/util/Delay.cpp
    lsp-dsp-units/src/main/util/Sidechain.cpp
    lsp-dsp-units/src/main/util/RawRingBuffer.cpp
    lsp-dsp-units/src/main/ctl/Bypass.cpp
    lsp-dsp-units/src/main/filters/Equalizer.cpp
    lsp-dsp-units/src/main/filters/Filter.cpp
    lsp-dsp-units/src/main/filters/FilterBank.cpp
    lsp-dsp-units/src/main/misc/windows.cpp
    lsp-common-lib/src/main/singletone.cpp
    lsp-common-lib/src/main/debug.cpp
    lsp-common-lib/src/main/status.cpp
    lsp-common-lib/src/main/bits.cpp
)
add_library(lsp_dsp_obj OBJECT ${LSP_DSP_SOURCES})
target_include_directories(lsp_dsp_obj PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/lsp-dsp-units/include
    ${CMAKE_CURRENT_SOURCE_DIR}/lsp-dsp-lib/include
    ${CMAKE_CURRENT_SOURCE_DIR}/lsp-runtime/include
    ${CMAKE_CURRENT_SOURCE_DIR}/lsp-common-lib/include
)
target_compile_definitions(lsp_dsp_obj PRIVATE LSP_DSP_UNITS_PUBLIC=)

# Lua 5.4.8 — embedded scripting engine
file(GLOB LUA_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_LIST_DIR}/lua/src/lapi.c     ${CMAKE_CURRENT_LIST_DIR}/lua/src/lauxlib.c
    ${CMAKE_CURRENT_LIST_DIR}/lua/src/lbaselib.c  ${CMAKE_CURRENT_LIST_DIR}/lua/src/lcode.c
    ${CMAKE_CURRENT_LIST_DIR}/lua/src/lcorolib.c  ${CMAKE_CURRENT_LIST_DIR}/lua/src/lctype.c
    ${CMAKE_CURRENT_LIST_DIR}/lua/src/ldblib.c    ${CMAKE_CURRENT_LIST_DIR}/lua/src/ldebug.c
    ${CMAKE_CURRENT_LIST_DIR}/lua/src/ldo.c       ${CMAKE_CURRENT_LIST_DIR}/lua/src/ldump.c
    ${CMAKE_CURRENT_LIST_DIR}/lua/src/lfunc.c     ${CMAKE_CURRENT_LIST_DIR}/lua/src/lgc.c
    ${CMAKE_CURRENT_LIST_DIR}/lua/src/linit.c     ${CMAKE_CURRENT_LIST_DIR}/lua/src/liolib.c
    ${CMAKE_CURRENT_LIST_DIR}/lua/src/llex.c      ${CMAKE_CURRENT_LIST_DIR}/lua/src/lmathlib.c
    ${CMAKE_CURRENT_LIST_DIR}/lua/src/lmem.c      ${CMAKE_CURRENT_LIST_DIR}/lua/src/loadlib.c
    ${CMAKE_CURRENT_LIST_DIR}/lua/src/lobject.c   ${CMAKE_CURRENT_LIST_DIR}/lua/src/lopcodes.c
    ${CMAKE_CURRENT_LIST_DIR}/lua/src/loslib.c    ${CMAKE_CURRENT_LIST_DIR}/lua/src/lparser.c
    ${CMAKE_CURRENT_LIST_DIR}/lua/src/lstate.c    ${CMAKE_CURRENT_LIST_DIR}/lua/src/lstring.c
    ${CMAKE_CURRENT_LIST_DIR}/lua/src/lstrlib.c   ${CMAKE_CURRENT_LIST_DIR}/lua/src/ltable.c
    ${CMAKE_CURRENT_LIST_DIR}/lua/src/ltablib.c   ${CMAKE_CURRENT_LIST_DIR}/lua/src/ltm.c
    ${CMAKE_CURRENT_LIST_DIR}/lua/src/lundump.c   ${CMAKE_CURRENT_LIST_DIR}/lua/src/lutf8lib.c
    ${CMAKE_CURRENT_LIST_DIR}/lua/src/lvm.c       ${CMAKE_CURRENT_LIST_DIR}/lua/src/lzio.c
)

# JamesDSP core (C) — exclude replaced modules
file(GLOB_RECURSE LIBJAMESDSP_SOURCE_FILES CONFIGURE_DEPENDS
        ${CMAKE_CURRENT_LIST_DIR}/libjamesdsp/Main/libjamesdsp/jni/jamesdsp/jdsp/*.c
        ${CMAKE_CURRENT_LIST_DIR}/libjamesdsp/Main/libjamesdsp/jni/jamesdsp/jdsp/*.h)
# The AIDL entry point file is one level above the jdsp/ directory
list(APPEND LIBJAMESDSP_SOURCE_FILES ${CMAKE_CURRENT_LIST_DIR}/libjamesdsp/Main/libjamesdsp/jni/jamesdsp/jamesdsp.c)
# dbb.c (Dynamic Bass Boost) restored 2026-08-07 as an opt-in module — no
# longer excluded. dynamic.c's old FFTCompander-based Compressor stays
# excluded; it was replaced by the LSP compressor port, not just deprioritized.
list(FILTER LIBJAMESDSP_SOURCE_FILES EXCLUDE REGEX "Effects/dynamic\\.c$")

# Main shared library
add_library(jamesdsp SHARED
    ${LIBJAMESDSP_SOURCE_FILES}
    EELStdOutExtension.c EELStdOutExtension.h
    lua_bridge.c
    lsp_wrap.cpp
    chain_modules.c
    $<TARGET_OBJECTS:lsp_dsp_obj>
    ${LUA_SOURCES})

target_compile_options(jamesdsp PRIVATE
    $<$<COMPILE_LANGUAGE:C>:-std=gnu11 -O2 -Wno-incompatible-function-pointer-types -Wno-incompatible-pointer-types -Wno-implicit-int -Wno-implicit-function-declaration>
    $<$<COMPILE_LANGUAGE:CXX>:-std=c++17>)
target_include_directories(jamesdsp PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/libjamesdsp/Main/libjamesdsp/jni/jamesdsp/jdsp/)
target_include_directories(jamesdsp PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/lua/src)
target_include_directories(jamesdsp PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/lsp-dsp-units/include)
target_include_directories(jamesdsp PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/lsp-dsp-lib/include)
target_include_directories(jamesdsp PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/lsp-runtime/include)
target_include_directories(jamesdsp PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/lsp-common-lib/include)
target_include_directories(jamesdsp PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(jamesdsp PRIVATE LUA_COMPAT_5_3 LUA_USE_POSIX)
set_target_properties(jamesdsp PROPERTIES OUTPUT_NAME jamesdsp_engine)

find_library(log-lib log)
target_link_libraries(jamesdsp PRIVATE ${log-lib})
