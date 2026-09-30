# Game pack: the player's own translated game code as one library.
#
# Built by PadForge from the player's disc; loaded by the published app
# (MKW_GAME_PACK=APP, see include/game_pack.h). Needs only a compiler and this
# runtime's headers: no graphics or third-party libraries are built.
#
#   cmake -S runtime/game_pack -B <build> \
#     -DMKW_TRANSLATED_SHARD_MANIFEST=<translation>/build_shards/shards.cmake \
#     -DMKW_GAME_PACK_RUNTIME=<the app's runtime library, e.g. libmain.so> \
#     -DMKW_KARTPAD_RUNTIME_INCLUDE=<kartpad>/runtime/include \
#     -DKARTPAD_APP_VERSION=<app version> [-DMKW_GAME_PACK_DEFINITIONS=...]
cmake_minimum_required(VERSION 3.24)
project(kartpad_game_pack LANGUAGES C CXX ASM)

set(MKW_TRANSLATED_SHARD_MANIFEST "" CACHE FILEPATH "Translator-owned aggregate shard manifest")
set(MKW_GAME_PACK_RUNTIME "" CACHE FILEPATH "The published app's runtime library to link against")
set(MKW_KARTPAD_RUNTIME_INCLUDE "" CACHE PATH "KartPad's header-only PowerPC semantics")
set(KARTPAD_APP_VERSION "" CACHE STRING "App version this pack is built for")
set(KARTPAD_PACK_FINGERPRINT "" CACHE STRING "Pack interface fingerprint (kartpad_builder.pack_fingerprint)")
# Must equal the app build's platform definitions: they shape code inlined from
# the runtime headers into every translated function.
set(MKW_GAME_PACK_DEFINITIONS "" CACHE STRING "Platform compile definitions of the app build")

foreach(required MKW_TRANSLATED_SHARD_MANIFEST MKW_GAME_PACK_RUNTIME MKW_KARTPAD_RUNTIME_INCLUDE)
    if(NOT EXISTS "${${required}}")
        message(FATAL_ERROR "${required} must name an existing path")
    endif()
endforeach()
if(NOT KARTPAD_APP_VERSION)
    message(FATAL_ERROR "KARTPAD_APP_VERSION is required")
endif()
if(NOT KARTPAD_PACK_FINGERPRINT MATCHES "^[0-9a-f]+$")
    message(FATAL_ERROR "KARTPAD_PACK_FINGERPRINT is required (lowercase hex)")
endif()
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8 OR NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64|ARM64)$")
    message(FATAL_ERROR "Game packs are built for 64-bit arm64 only")
endif()

get_filename_component(MKW_RUNTIME_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
include("${MKW_TRANSLATED_SHARD_MANIFEST}")
get_filename_component(MKW_TRANSLATION_ROOT "${MKW_TRANSLATED_SHARD_MANIFEST}" DIRECTORY)
get_filename_component(MKW_TRANSLATION_ROOT "${MKW_TRANSLATION_ROOT}" DIRECTORY)
# pack_info.cpp includes "generated/RuntimeConfig.h"; copy it rather than
# relying on a symlinked workspace, which Windows handles poorly.
configure_file("${MKW_TRANSLATION_ROOT}/RuntimeConfig.h"
    "${CMAKE_CURRENT_BINARY_DIR}/include/generated/RuntimeConfig.h" COPYONLY)

set(pack_sources
    ${MKW_BASE_COMMON_SHARDS} ${MKW_BASE_PORTABLE_SENSITIVE_SHARDS} ${MKW_BASE_REGISTRATION_SOURCES}
    "${MKW_TRANSLATION_ROOT}/data_sections_init.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/pack_info.cpp")
if(MKW_HAVE_RETRO_REWIND_SHARDS)
    list(APPEND pack_sources
        ${MKW_RETRO_PORTABLE_SENSITIVE_SHARDS} ${MKW_RETRO_MOD_SHARDS} ${MKW_RETRO_REGISTRATION_SOURCES})
endif()

# Data blobs: the translator may run on a Mac and emit Mach-O section syntax.
set(blob_sources "${MKW_TRANSLATION_ROOT}/data_sections_init_blobs.S")
if(MKW_HAVE_RETRO_REWIND_SHARDS)
    foreach(source IN LISTS MKW_RETRO_EXTRA_SOURCES)
        if(source MATCHES "\\.S$")
            list(APPEND blob_sources "${source}")
        else()
            list(APPEND pack_sources "${source}")
        endif()
    endforeach()
endif()
foreach(source IN LISTS blob_sources)
    if(NOT EXISTS "${source}")
        continue()
    endif()
    if(APPLE)
        list(APPEND pack_sources "${source}")
        continue()
    endif()
    file(READ "${source}" content)
    string(REPLACE ".section __TEXT,__const" ".section .rodata,\"a\",%progbits" content "${content}")
    string(REPLACE ".section .rdata,\"dr\"" ".section .rodata,\"a\",%progbits" content "${content}")
    if(content MATCHES "\n\\.globl k")
        # Both spellings present (the builder adds _k aliases for Mach-O
        # consumers): ELF needs only the plain names.
        string(REGEX REPLACE "\n\\.globl _k[^\n]*" "" content "${content}")
        string(REGEX REPLACE "\n_k[^\n]*:" "" content "${content}")
    else()
        string(REPLACE ".globl _k" ".globl k" content "${content}")
        string(REPLACE "\n_k" "\nk" content "${content}")
    endif()
    get_filename_component(name "${source}" NAME)
    set(normalized "${CMAKE_CURRENT_BINARY_DIR}/elf_${name}")
    file(WRITE "${normalized}" "${content}")
    list(APPEND pack_sources "${normalized}")
endforeach()
foreach(source IN LISTS pack_sources)
    if(source MATCHES "\\.S$")
        set_source_files_properties("${source}" PROPERTIES LANGUAGE ASM)
    endif()
endforeach()

add_library(kartpad_game SHARED ${pack_sources})
set_target_properties(kartpad_game PROPERTIES
    CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON
    CXX_VISIBILITY_PRESET hidden C_VISIBILITY_PRESET hidden
    POSITION_INDEPENDENT_CODE ON)
target_include_directories(kartpad_game PRIVATE
    "${CMAKE_CURRENT_BINARY_DIR}/include"
    "${MKW_RUNTIME_SOURCE_DIR}/include"
    "${MKW_RUNTIME_SOURCE_DIR}/src"
    "${MKW_RUNTIME_SOURCE_DIR}/../aurora-main/include")
target_include_directories(kartpad_game SYSTEM PRIVATE "${MKW_KARTPAD_RUNTIME_INCLUDE}")
target_compile_definitions(kartpad_game PRIVATE
    NOMINMAX TARGET_PC MKW_GAME_PACK_MODULE=1
    "KARTPAD_APP_VERSION=\"${KARTPAD_APP_VERSION}\""
    "KARTPAD_PACK_FINGERPRINT=\"${KARTPAD_PACK_FINGERPRINT}\"" ${MKW_GAME_PACK_DEFINITIONS})
# Same options as the runtime's translated targets (cmake/PublicProducts.cmake).
target_compile_options(kartpad_game PRIVATE
    -O2 -fno-fast-math -ffp-contract=off -fno-slp-vectorize -w -pipe
    "-ffile-prefix-map=${MKW_RUNTIME_SOURCE_DIR}=wiicompiled-runtime"
    "-ffile-prefix-map=${MKW_TRANSLATION_ROOT}=translation")
if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    target_compile_options(kartpad_game PRIVATE "SHELL:-mcpu=generic -Xclang -target-feature -Xclang -rcpc")
    target_link_options(kartpad_game PRIVATE "-Wl,-undefined,dynamic_lookup")
elseif(APPLE)
    target_compile_options(kartpad_game PRIVATE -mcpu=apple-m2)
    target_link_options(kartpad_game PRIVATE "-Wl,-undefined,dynamic_lookup")
else()
    # Every symbol must resolve against the app's runtime library.
    target_link_libraries(kartpad_game PRIVATE "${MKW_GAME_PACK_RUNTIME}")
    target_link_options(kartpad_game PRIVATE "-Wl,-z,defs")
endif()
