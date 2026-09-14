# Public WiiCompiled product graph.
#
# The translator owns the translated build graph. Mario Kart's profile-neutral
# functions are compiled once into mkw_base_shared; only callers whose direct
# ABI differs between profiles receive small base/RR variants.

set(DATA_INIT_FILE "${MKW_RUNTIME_SOURCE_DIR}/../generated/data_sections_init.cpp")
set(DATA_INIT_BLOB_ASM "${MKW_RUNTIME_SOURCE_DIR}/../generated/data_sections_init_blobs.S")
if(EXISTS "${DATA_INIT_FILE}")
    list(APPEND SOURCES "${DATA_INIT_FILE}")
endif()
# Crash-report symbolization table emitted by generate-data-init. The stub
# (deliberately outside the globbed src/ tree so it is never picked up twice)
# keeps link succeeding when the generated table has not been produced yet.
set(GUEST_SYMBOL_TABLE_FILE "${MKW_RUNTIME_SOURCE_DIR}/../generated/guest_symbol_table.cpp")
if(EXISTS "${GUEST_SYMBOL_TABLE_FILE}")
    list(APPEND SOURCES "${GUEST_SYMBOL_TABLE_FILE}")
else()
    list(APPEND SOURCES "${MKW_RUNTIME_SOURCE_DIR}/cmake/guest_symbol_table_stub.cpp")
endif()
if(EXISTS "${DATA_INIT_BLOB_ASM}")
    enable_language(ASM)
    set_source_files_properties("${DATA_INIT_BLOB_ASM}" PROPERTIES LANGUAGE ASM SKIP_UNITY_BUILD_INCLUSION ON)
    list(APPEND SOURCES "${DATA_INIT_BLOB_ASM}")
endif()
list(REMOVE_DUPLICATES SOURCES)

function(mkw_apply_common_compile_options target)
    target_compile_options(${target} PRIVATE -O3 -ffast-math -w -pipe)
endfunction()

function(mkw_apply_translated_compile_options target)
    target_compile_options(${target} PRIVATE
        -O2 ${MKW_TRANSLATED_PPC_FP_OPTIONS} -fno-slp-vectorize -w -pipe)
endfunction()

function(mkw_configure_object_target target)
    target_include_directories(${target} PRIVATE
        "${MKW_RUNTIME_SOURCE_DIR}/include"
        "${MKW_RUNTIME_SOURCE_DIR}/src"
        # Workspace root, so translator output is spelled "generated/<x>.h"
        # instead of a ../ chain whose depth depends on the includer.
        "${MKW_RUNTIME_SOURCE_DIR}/.."
        "${MKW_RUNTIME_SOURCE_DIR}/../aurora-main/include")
    target_compile_definitions(${target} PRIVATE
        TARGET_PC)
    set_target_properties(${target} PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
endfunction()

# Translated shard TUs are the memory-hungry compiles; everything else in the build is
# comparatively small. A dedicated Ninja job pool caps how many of them run at once so the
# global parallelism can use every core for the cheap TUs without the memory ceiling being a
# guess. The pool depth is per-machine (derived from installed RAM by LocalBuild.ps1) and is
# deliberately not part of the canonical flag set: it changes scheduling, never output bytes.
if(MKW_TRANSLATED_COMPILE_JOBS GREATER 0)
    set_property(GLOBAL APPEND PROPERTY JOB_POOLS "mkw_translated=${MKW_TRANSLATED_COMPILE_JOBS}")
endif()

function(mkw_bound_translated_compiles target)
    if(MKW_TRANSLATED_COMPILE_JOBS GREATER 0)
        set_property(TARGET ${target} PROPERTY JOB_POOL_COMPILE mkw_translated)
    endif()
endfunction()

function(mkw_configure_translated_target target)
    mkw_configure_object_target(${target})
    mkw_apply_translated_compile_options(${target})
    mkw_bound_translated_compiles(${target})
endfunction()

add_library(mkw_runtime_common OBJECT ${SOURCES})
mkw_configure_object_target(mkw_runtime_common)
target_compile_features(mkw_runtime_common PRIVATE cxx_std_20)
target_compile_definitions(mkw_runtime_common PRIVATE
    $<$<NOT:$<PLATFORM_ID:iOS>>:SDL_MAIN_HANDLED>
    _DISABLE_STRING_ANNOTATION _DISABLE_VECTOR_ANNOTATION
    $<$<PLATFORM_ID:Darwin,iOS>:_XOPEN_SOURCE>)
target_link_libraries(mkw_runtime_common PRIVATE
    aurora::gx aurora::pad aurora::si aurora::vi aurora::mtx)
target_link_libraries(mkw_runtime_common PRIVATE mkw::pugixml mkw::toml11 mkw::cryptopp)
target_link_libraries(mkw_runtime_common PRIVATE "-framework Foundation")
if(MKW_CPPWINRT_INCLUDE_DIR)
    if(NOT EXISTS "${MKW_CPPWINRT_INCLUDE_DIR}/winrt/base.h")
        message(FATAL_ERROR
            "MKW_CPPWINRT_INCLUDE_DIR does not contain winrt/base.h: ${MKW_CPPWINRT_INCLUDE_DIR}")
    endif()
    target_include_directories(mkw_runtime_common PRIVATE "${MKW_CPPWINRT_INCLUDE_DIR}")
endif()

# Keep runtime unity units small and semantically related. The old generated-TU
# batch size put all 57 native runtime sources into one memory-heavy compiler job.
foreach(source IN LISTS SOURCES)
    get_filename_component(source_name "${source}" NAME_WE)
    string(REPLACE "\\" "/" source_normalized "${source}")
    if(source_normalized MATCHES "/hle/gx/")
        set(runtime_group "gx_bridge")
    elseif(source_name MATCHES "network|socket|dns|dwc|ios")
        set(runtime_group "network_ios")
    elseif(source_name MATCHES "os_|system|memory|fiber|scheduler")
        set(runtime_group "guest_system")
    elseif(source_name MATCHES "debug|trace|prof")
        set(runtime_group "diagnostics")
    else()
        string(SHA256 source_hash "${source_name}")
        string(SUBSTRING "${source_hash}" 0 4 source_hash_prefix)
        math(EXPR runtime_bucket "0x${source_hash_prefix} % 8")
        set(runtime_group "runtime_${runtime_bucket}")
    endif()
    set_source_files_properties("${source}" PROPERTIES UNITY_GROUP "${runtime_group}")
endforeach()
# These translation units implement guest-visible floating-point bit
# semantics.  Keep them out of the fast-math runtime unity groups and apply
# the same contraction/rounding policy as translated PPC shards.
set(MKW_PPC_SEMANTIC_RUNTIME_SOURCES
    "${MKW_RUNTIME_SOURCE_DIR}/src/ppc_helpers.cpp"
    "${MKW_RUNTIME_SOURCE_DIR}/src/fpu_helpers.cpp")
set_source_files_properties(${MKW_PPC_SEMANTIC_RUNTIME_SOURCES} PROPERTIES
    SKIP_UNITY_BUILD_INCLUSION ON
    SKIP_PRECOMPILE_HEADERS ON
    COMPILE_OPTIONS "${MKW_TRANSLATED_PPC_FP_OPTIONS}")
set_target_properties(mkw_runtime_common PROPERTIES UNITY_BUILD ON UNITY_BUILD_MODE GROUP)
target_precompile_headers(mkw_runtime_common PRIVATE "${MKW_RUNTIME_SOURCE_DIR}/include/mkw_pch.h")
mkw_apply_common_compile_options(mkw_runtime_common)

# Host ISA guard. Everything in MKW_ALL_BUILD_TARGETS below is compiled with
# -march=x86-64-v3; this object library deliberately is not, which
# is the whole point of keeping it out of mkw_runtime_common. It runs a CPUID
# check from a C initializer so an unsupported machine gets a readable error
# instead of an illegal-instruction crash. Excluded from the unity build and the
# precompiled header because both are produced with the owning target's flags.
add_library(mkw_cpu_baseline OBJECT "${MKW_RUNTIME_SOURCE_DIR}/src/apple/host_cpu_baseline_stub.cpp")
target_compile_features(mkw_cpu_baseline PRIVATE cxx_std_17)
set_target_properties(mkw_cpu_baseline PROPERTIES UNITY_BUILD OFF)
target_compile_options(mkw_cpu_baseline PRIVATE -w)

if(NOT MKW_BASE_COMMON_SHARDS)
    message(FATAL_ERROR "Translator build graph contains no shared base shards")
endif()

add_library(mkw_base_shared STATIC ${MKW_BASE_COMMON_SHARDS})
mkw_configure_translated_target(mkw_base_shared)
target_precompile_headers(mkw_base_shared PRIVATE "${MKW_RUNTIME_SOURCE_DIR}/include/mkw_pch.h")

if(MKW_BASE_PORTABLE_SENSITIVE_SHARDS)
    add_library(mkw_base_sensitive OBJECT ${MKW_BASE_PORTABLE_SENSITIVE_SHARDS})
    mkw_configure_translated_target(mkw_base_sensitive)
    target_precompile_headers(mkw_base_sensitive REUSE_FROM mkw_base_shared)
endif()

if(MKW_HAVE_RETRO_REWIND)
    if(MKW_RETRO_PORTABLE_SENSITIVE_SHARDS)
        add_library(mkw_retro_sensitive OBJECT ${MKW_RETRO_PORTABLE_SENSITIVE_SHARDS})
        mkw_configure_translated_target(mkw_retro_sensitive)
        target_precompile_headers(mkw_retro_sensitive REUSE_FROM mkw_base_shared)
    endif()

    set(MKW_RETRO_TRANSLATED_SOURCES ${MKW_RETRO_MOD_SHARDS} ${MKW_RETRO_EXTRA_SOURCES})
    set(MKW_RETRO_BLOB_OBJECTS)
    foreach(source IN LISTS MKW_RETRO_EXTRA_SOURCES)
        if(source MATCHES "\\.S$")
            enable_language(ASM)
            set_source_files_properties("${source}" PROPERTIES LANGUAGE ASM SKIP_PRECOMPILE_HEADERS ON)
        endif()
    endforeach()
    add_library(mkw_retro_rewind_functions OBJECT ${MKW_RETRO_TRANSLATED_SOURCES})
    mkw_configure_translated_target(mkw_retro_rewind_functions)
    target_precompile_headers(mkw_retro_rewind_functions REUSE_FROM mkw_base_shared)
endif()

function(mkw_configure_product target)
    target_sources(${target} PRIVATE $<TARGET_OBJECTS:mkw_runtime_common>)
    # Startup CPU check. Must stay a separate object library so it keeps the
    # plain baseline ISA while everything around it is built for x86-64-v3.
    target_sources(${target} PRIVATE $<TARGET_OBJECTS:mkw_cpu_baseline>)
    target_include_directories(${target} PRIVATE
        "${MKW_RUNTIME_SOURCE_DIR}/include"
        "${MKW_RUNTIME_SOURCE_DIR}/src"
        # Workspace root, so translator output is spelled "generated/<x>.h"
        # instead of a ../ chain whose depth depends on the includer.
        "${MKW_RUNTIME_SOURCE_DIR}/.."
        "${MKW_RUNTIME_SOURCE_DIR}/../aurora-main/include")
    target_compile_definitions(${target} PRIVATE
        $<$<NOT:$<PLATFORM_ID:iOS>>:SDL_MAIN_HANDLED>
        _DISABLE_STRING_ANNOTATION _DISABLE_VECTOR_ANNOTATION TARGET_PC)
    target_compile_features(${target} PRIVATE cxx_std_20)
    mkw_apply_common_compile_options(${target})
    # The dispatch-table and registration shards compile inside the product target itself and
    # include the same fat translated headers; bound them by the same pool.
    mkw_bound_translated_compiles(${target})
    target_link_libraries(${target} PRIVATE
        mkw_base_shared mkw::pugixml mkw::toml11 mkw::cryptopp)

    target_link_libraries(${target} PRIVATE
        aurora::gx aurora::pad aurora::si aurora::vi aurora::mtx)
    if(EXISTS "${MKW_AURORA_DIR}/cmake/AuroraCopyRuntimeDLLs.cmake")
        include("${MKW_AURORA_DIR}/cmake/AuroraCopyRuntimeDLLs.cmake")
        aurora_copy_runtime_dlls(${target})
    endif()
    if(TARGET sqlite3)
        get_target_property(MKW_SQLITE_TARGET_TYPE sqlite3 TYPE)
    endif()
    if(TARGET sqlite3 AND
       (MKW_SQLITE_TARGET_TYPE STREQUAL "SHARED_LIBRARY" OR
        MKW_SQLITE_TARGET_TYPE STREQUAL "MODULE_LIBRARY"))
        add_custom_command(TARGET ${target} POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:sqlite3> $<TARGET_FILE_DIR:${target}>)
    endif()

    target_link_libraries(${target} PRIVATE
        "-framework Foundation" "-framework CoreAudio" "-framework AudioToolbox"
        "-framework Security")

    if(WIN32)
      set_target_properties(${target} PROPERTIES WIN32_EXECUTABLE TRUE)
      foreach(runtime_dll libc++.dll libunwind.dll)
        execute_process(
            COMMAND "${CMAKE_CXX_COMPILER}" "--print-file-name=${runtime_dll}"
            OUTPUT_VARIABLE runtime_dll_path
            OUTPUT_STRIP_TRAILING_WHITESPACE)
        if(NOT EXISTS "${runtime_dll_path}")
            get_filename_component(mkw_compiler_bin "${CMAKE_CXX_COMPILER}" DIRECTORY)
            set(runtime_dll_path "${mkw_compiler_bin}/${runtime_dll}")
        endif()
        if(NOT EXISTS "${runtime_dll_path}")
            message(FATAL_ERROR "llvm-mingw runtime DLL not found: ${runtime_dll}")
        endif()
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${runtime_dll_path}" $<TARGET_FILE_DIR:${target}>)
      endforeach()
    endif()

    set(MKW_WII_BOOTSTRAP_SOURCE_DIR "${MKW_RUNTIME_SOURCE_DIR}/assets/wii")
    if(NOT EXISTS "${MKW_WII_BOOTSTRAP_SOURCE_DIR}/shared2/wc24")
        message(FATAL_ERROR "Missing Wii first-run bootstrap payload: ${MKW_WII_BOOTSTRAP_SOURCE_DIR}")
    endif()
    add_custom_command(TARGET ${target} POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${MKW_WII_BOOTSTRAP_SOURCE_DIR}" "$<TARGET_FILE_DIR:${target}>/wii_bootstrap")

    set(MKW_DSP_COEFFICIENT_ROM "${MKW_RUNTIME_SOURCE_DIR}/assets/dsp/dsp_coef.bin")
    if(NOT EXISTS "${MKW_DSP_COEFFICIENT_ROM}")
        message(FATAL_ERROR "Missing Wii DSP coefficient ROM: ${MKW_DSP_COEFFICIENT_ROM}")
    endif()
    file(SHA256 "${MKW_DSP_COEFFICIENT_ROM}" MKW_DSP_COEFFICIENT_ROM_SHA256)
    if(NOT MKW_DSP_COEFFICIENT_ROM_SHA256 STREQUAL
       "d7741279c2e8ec5c5fb318f8fbdd6de6bf583520d288e836a5383233a4238179")
        message(FATAL_ERROR "Wii DSP coefficient ROM hash mismatch: ${MKW_DSP_COEFFICIENT_ROM_SHA256}")
    endif()
    add_custom_command(TARGET ${target} POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${MKW_DSP_COEFFICIENT_ROM}" "$<TARGET_FILE_DIR:${target}>/dsp_coef.bin")

    # Aurora imports this portable recipe database into each user's writable
    # pipeline cache. Keep the upstream filename so its default resourcesPath
    # lookup works without application-specific configuration.
    set(MKW_INITIAL_PIPELINE_CACHE
        "${MKW_RUNTIME_SOURCE_DIR}/assets/pipeline/initial_pipeline_cache.db")
    if(NOT EXISTS "${MKW_INITIAL_PIPELINE_CACHE}")
        message(FATAL_ERROR "Missing transferable Aurora pipeline cache: ${MKW_INITIAL_PIPELINE_CACHE}")
    endif()
    add_custom_command(TARGET ${target} POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${MKW_INITIAL_PIPELINE_CACHE}"
        "$<TARGET_FILE_DIR:${target}>/initial_pipeline_cache.db")
endfunction()

add_executable(WiiCompiled "${MKW_BASE_PRODUCT_SOURCE}" ${MKW_BASE_REGISTRATION_SOURCES})
mkw_configure_product(WiiCompiled)
target_precompile_headers(WiiCompiled PRIVATE
    "${MKW_RUNTIME_SOURCE_DIR}/include/mkw_pch.h")
if(TARGET mkw_base_sensitive)
    target_sources(WiiCompiled PRIVATE $<TARGET_OBJECTS:mkw_base_sensitive>)
endif()

set(MKW_KARTPAD_REPO_ROOT "" CACHE PATH
    "KartPad repository root used to embed the exact mobile UIKit host")
set(MKW_KARTPAD_DISCIO_SOURCE_DIR "" CACHE PATH
    "Patched pinned Dolphin source used by KartPad's iOS WBFS importer")
set(MKW_KARTPAD_DISCIO_BUILD_DIR "" CACHE PATH
    "Matching iOS Dolphin build containing KartPad's DiscIO dependency graph")
if(CMAKE_SYSTEM_NAME STREQUAL "iOS" AND MKW_KARTPAD_REPO_ROOT)
    set(MKW_KARTPAD_IOS_DIR "${MKW_KARTPAD_REPO_ROOT}/apple/ios")
    set(MKW_KARTPAD_MOBILE_DIR "${MKW_KARTPAD_REPO_ROOT}/apple/mobile")
    set(MKW_KARTPAD_SHARED_DIR "${MKW_KARTPAD_REPO_ROOT}/apple/shared")
    set(MKW_KARTPAD_SUNPAD_DIR "${MKW_KARTPAD_REPO_ROOT}/apple/third_party/sunpad")
    set(MKW_KARTPAD_ICON_CATALOG "${MKW_KARTPAD_IOS_DIR}/Assets.xcassets")
    set(MKW_KARTPAD_PRIVACY_MANIFEST "${MKW_KARTPAD_IOS_DIR}/PrivacyInfo.xcprivacy")
    if(NOT EXISTS "${MKW_KARTPAD_DISCIO_SOURCE_DIR}/Source/Core/DiscIO/DiscExtractor.h")
        message(FATAL_ERROR "Missing patched pinned Dolphin source for iOS WBFS import")
    endif()
    set(MKW_KARTPAD_DISCIO_ARCHIVES
        "${MKW_KARTPAD_DISCIO_BUILD_DIR}/Source/Core/DiscIO/libdiscio.a"
        "${MKW_KARTPAD_DISCIO_BUILD_DIR}/Externals/bzip2/libbzip2.a"
        "${MKW_KARTPAD_DISCIO_BUILD_DIR}/Externals/LZO/liblzo2.a"
        "${MKW_KARTPAD_DISCIO_BUILD_DIR}/Externals/lz4/lz4/build/cmake/liblz4.a"
        "${MKW_KARTPAD_DISCIO_BUILD_DIR}/Externals/xxhash/libxxhash.a"
        "${MKW_KARTPAD_DISCIO_BUILD_DIR}/Externals/pugixml/pugixml/libpugixml.a"
        "${MKW_KARTPAD_DISCIO_BUILD_DIR}/Source/Core/Common/libcommon.a"
        "${MKW_KARTPAD_DISCIO_BUILD_DIR}/Externals/fmt/fmt/libfmt.a"
        "${MKW_KARTPAD_DISCIO_BUILD_DIR}/Externals/minizip-ng/minizip-ng/libminizip-ng.a"
        "${MKW_KARTPAD_DISCIO_BUILD_DIR}/Externals/liblzma/liblzma.a"
        "${MKW_KARTPAD_DISCIO_BUILD_DIR}/Externals/zstd/zstd/build/cmake/lib/libzstd.a"
        "${MKW_KARTPAD_DISCIO_BUILD_DIR}/Externals/enet/enet/libenet.a"
        "${MKW_KARTPAD_DISCIO_BUILD_DIR}/Externals/SFML/libsfml-network.a"
        "${MKW_KARTPAD_DISCIO_BUILD_DIR}/Externals/SFML/libsfml-system.a"
        "${MKW_KARTPAD_DISCIO_BUILD_DIR}/Externals/FatFs/libFatFs.a"
        "${MKW_KARTPAD_DISCIO_BUILD_DIR}/Externals/libiconv/libiconv.a"
        "${MKW_KARTPAD_DISCIO_BUILD_DIR}/Externals/libiconv/libcharset/liblibcharset.a"
        "${MKW_KARTPAD_DISCIO_BUILD_DIR}/Externals/curl/curl/lib/libcurl.a"
        "${MKW_KARTPAD_DISCIO_BUILD_DIR}/Externals/mbedtls/library/libmbedtls.a"
        "${MKW_KARTPAD_DISCIO_BUILD_DIR}/Externals/mbedtls/library/libmbedx509.a"
        "${MKW_KARTPAD_DISCIO_BUILD_DIR}/Externals/mbedtls/library/libmbedcrypto.a"
        "${MKW_KARTPAD_DISCIO_BUILD_DIR}/Externals/libspng/libspng/libspng_static.a"
        "${MKW_KARTPAD_DISCIO_BUILD_DIR}/Externals/zlib-ng/zlib-ng/libz.a")
    foreach(archive IN LISTS MKW_KARTPAD_DISCIO_ARCHIVES)
        if(NOT EXISTS "${archive}")
            message(FATAL_ERROR "Missing iOS DiscIO archive: ${archive}")
        endif()
    endforeach()
    set(MKW_KARTPAD_MOBILE_SOURCES
        "${MKW_KARTPAD_IOS_DIR}/KartPadRuntimeOverlayHost.mm"
        "${MKW_KARTPAD_SHARED_DIR}/KartPadMiiManager.mm"
        "${MKW_KARTPAD_IOS_DIR}/KartPadDiscExtractor.mm"
        "${MKW_KARTPAD_IOS_DIR}/KartPadDiscFormats.cpp"
        "${MKW_KARTPAD_IOS_DIR}/KartPadRetroRewindInstaller.mm"
        "${MKW_KARTPAD_REPO_ROOT}/runtime/src/retro_rewind/archive_path.cpp"
        "${MKW_KARTPAD_REPO_ROOT}/runtime/src/retro_rewind/archive_scan.cpp"
        "${MKW_KARTPAD_MOBILE_DIR}/KartPadClassicInput.mm"
        "${MKW_KARTPAD_MOBILE_DIR}/KartPadPhysicalControllers.mm"
        "${MKW_KARTPAD_MOBILE_DIR}/KartPadMotionSteering.mm"
        "${MKW_KARTPAD_SUNPAD_DIR}/SunPadControllerMapping.mm"
        "${MKW_KARTPAD_SUNPAD_DIR}/SunPadGameOverlay.mm"
        "${MKW_KARTPAD_SUNPAD_DIR}/SunPadInputMixer.mm"
        "${MKW_KARTPAD_SUNPAD_DIR}/SunPadSettings.mm"
        "${MKW_KARTPAD_SUNPAD_DIR}/SunPadDiagnostics.mm")
    foreach(source IN LISTS MKW_KARTPAD_MOBILE_SOURCES)
        if(NOT EXISTS "${source}")
            message(FATAL_ERROR "Missing KartPad mobile host source: ${source}")
        endif()
    endforeach()
    foreach(resource IN ITEMS MKW_KARTPAD_ICON_CATALOG MKW_KARTPAD_PRIVACY_MANIFEST)
        if(NOT EXISTS "${${resource}}")
            message(FATAL_ERROR "Missing KartPad mobile resource: ${${resource}}")
        endif()
    endforeach()

    target_sources(WiiCompiled PRIVATE ${MKW_KARTPAD_MOBILE_SOURCES}
        "${MKW_KARTPAD_ICON_CATALOG}" "${MKW_KARTPAD_PRIVACY_MANIFEST}")
    target_include_directories(WiiCompiled PRIVATE
        "${MKW_KARTPAD_IOS_DIR}"
        "${MKW_KARTPAD_MOBILE_DIR}"
        "${MKW_KARTPAD_SHARED_DIR}"
        "${MKW_KARTPAD_SUNPAD_DIR}"
        "${MKW_KARTPAD_REPO_ROOT}/runtime/include"
        "${MKW_KARTPAD_DISCIO_SOURCE_DIR}/Source/Core"
        "${MKW_KARTPAD_DISCIO_SOURCE_DIR}/Externals/fmt/fmt/include"
        "${MKW_KARTPAD_DISCIO_SOURCE_DIR}/Externals/minizip-ng/minizip-ng"
        "${CMAKE_CURRENT_LIST_DIR}/../third_party/kartpad-profile")
    set_source_files_properties(${MKW_KARTPAD_MOBILE_SOURCES}
        PROPERTIES
            COMPILE_OPTIONS "-fobjc-arc"
            SKIP_PRECOMPILE_HEADERS TRUE)
    set_source_files_properties(
        "${MKW_KARTPAD_IOS_DIR}/KartPadDiscExtractor.mm"
        "${MKW_KARTPAD_IOS_DIR}/KartPadDiscFormats.cpp"
        PROPERTIES
            COMPILE_OPTIONS "-fobjc-arc;-std=gnu++23"
            SKIP_PRECOMPILE_HEADERS TRUE)
    set_source_files_properties(
        "${MKW_KARTPAD_ICON_CATALOG}" "${MKW_KARTPAD_PRIVACY_MANIFEST}"
        PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
    target_link_options(WiiCompiled PRIVATE "-ObjC")
    target_link_libraries(WiiCompiled PRIVATE
        ${MKW_KARTPAD_DISCIO_ARCHIVES}
        "-framework UIKit" "-framework CoreGraphics" "-framework QuartzCore"
        "-framework Metal" "-framework GameController"
        "-framework CoreMotion" "-framework UniformTypeIdentifiers"
        "-framework SystemConfiguration" "-framework CoreFoundation"
        "-framework CoreServices" "-framework Foundation" "-framework SafariServices"
        "-lcompression" "-lresolv")
    set_target_properties(WiiCompiled PROPERTIES
        OUTPUT_NAME KartPad
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_INFO_PLIST "${MKW_KARTPAD_IOS_DIR}/RuntimeInfo.plist"
        XCODE_ATTRIBUTE_ARCHS arm64
        XCODE_ATTRIBUTE_ASSETCATALOG_COMPILER_APPICON_NAME AppIcon
        XCODE_ATTRIBUTE_CLANG_CXX_LANGUAGE_STANDARD "c++20"
        XCODE_ATTRIBUTE_CURRENT_PROJECT_VERSION 3
        XCODE_ATTRIBUTE_GENERATE_INFOPLIST_FILE NO
        XCODE_ATTRIBUTE_IPHONEOS_DEPLOYMENT_TARGET 16.0
        XCODE_ATTRIBUTE_MARKETING_VERSION 0.2.0
        XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER dev.kartpad.app
        XCODE_ATTRIBUTE_SUPPORTED_PLATFORMS "iphonesimulator iphoneos"
        XCODE_ATTRIBUTE_SUPPORTS_MACCATALYST NO
        XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1,2")
endif()

if(MKW_HAVE_RETRO_REWIND)
    add_executable(RetroRewind "${MKW_RETRO_REWIND_PRODUCT_SOURCE}" ${MKW_RETRO_REGISTRATION_SOURCES})
    mkw_configure_product(RetroRewind)
    target_precompile_headers(RetroRewind REUSE_FROM WiiCompiled)
    if(TARGET mkw_retro_sensitive)
        target_sources(RetroRewind PRIVATE $<TARGET_OBJECTS:mkw_retro_sensitive>)
    endif()
    target_sources(RetroRewind PRIVATE $<TARGET_OBJECTS:mkw_retro_rewind_functions>)
    if(MKW_RETRO_BLOB_OBJECTS)
        target_sources(RetroRewind PRIVATE ${MKW_RETRO_BLOB_OBJECTS})
    endif()
    if(APPLE AND NOT CMAKE_SYSTEM_NAME STREQUAL "iOS" AND
       DEFINED MKW_KARTPAD_MACOS_SHELL AND MKW_KARTPAD_MACOS_SHELL)
        target_sources(RetroRewind PRIVATE ${MKW_KARTPAD_MACOS_SOURCES})
        target_include_directories(RetroRewind PRIVATE
            "${MKW_KARTPAD_MACOS_DIR}" "${MKW_KARTPAD_REPO_ROOT}/apple/shared"
            "${MKW_KARTPAD_REPO_ROOT}/runtime/include")
        target_link_libraries(RetroRewind PRIVATE
            "-framework AppKit" "-framework UniformTypeIdentifiers"
            "-framework IOBluetooth" "-framework IOKit")
    endif()
    if(CMAKE_SYSTEM_NAME STREQUAL "iOS" AND
       DEFINED MKW_KARTPAD_MOBILE_SOURCES AND MKW_KARTPAD_MOBILE_SOURCES)
        # The base Apple patch also emits WiiCompiled as KartPad.app. Give the
        # unselected base product its own bundle path so Ninja does not see two
        # asset-catalog rules generating KartPad.app/Assets.xcassets.
        set_target_properties(WiiCompiled PROPERTIES OUTPUT_NAME WiiCompiledBase)
        target_sources(RetroRewind PRIVATE ${MKW_KARTPAD_MOBILE_SOURCES}
            "${MKW_KARTPAD_ICON_CATALOG}" "${MKW_KARTPAD_PRIVACY_MANIFEST}")
        target_include_directories(RetroRewind PRIVATE
            "${MKW_KARTPAD_IOS_DIR}"
            "${MKW_KARTPAD_MOBILE_DIR}"
            "${MKW_KARTPAD_SHARED_DIR}"
            "${MKW_KARTPAD_SUNPAD_DIR}"
            "${MKW_KARTPAD_REPO_ROOT}/runtime/include"
            "${MKW_KARTPAD_DISCIO_SOURCE_DIR}/Source/Core"
            "${MKW_KARTPAD_DISCIO_SOURCE_DIR}/Externals/fmt/fmt/include"
            "${MKW_KARTPAD_DISCIO_SOURCE_DIR}/Externals/minizip-ng/minizip-ng"
            "${CMAKE_CURRENT_LIST_DIR}/../third_party/kartpad-profile")
        target_link_options(RetroRewind PRIVATE "-ObjC")
        target_link_libraries(RetroRewind PRIVATE
            ${MKW_KARTPAD_DISCIO_ARCHIVES}
            "-framework UIKit" "-framework CoreGraphics" "-framework QuartzCore"
            "-framework Metal" "-framework GameController"
            "-framework CoreMotion" "-framework UniformTypeIdentifiers"
            "-framework SystemConfiguration" "-framework CoreFoundation"
            "-framework CoreServices" "-framework Foundation" "-framework SafariServices"
            "-lcompression" "-lresolv")
        set_target_properties(RetroRewind PROPERTIES
            OUTPUT_NAME KartPad
            MACOSX_BUNDLE TRUE
            MACOSX_BUNDLE_INFO_PLIST "${MKW_KARTPAD_IOS_DIR}/RuntimeInfo.plist"
            XCODE_ATTRIBUTE_ARCHS arm64
            XCODE_ATTRIBUTE_ASSETCATALOG_COMPILER_APPICON_NAME AppIcon
            XCODE_ATTRIBUTE_CLANG_CXX_LANGUAGE_STANDARD "c++20"
            XCODE_ATTRIBUTE_CURRENT_PROJECT_VERSION 3
            XCODE_ATTRIBUTE_GENERATE_INFOPLIST_FILE NO
            XCODE_ATTRIBUTE_IPHONEOS_DEPLOYMENT_TARGET 16.0
            XCODE_ATTRIBUTE_MARKETING_VERSION 0.2.0
            XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER dev.kartpad.app
            XCODE_ATTRIBUTE_SUPPORTED_PLATFORMS "iphonesimulator iphoneos"
            XCODE_ATTRIBUTE_SUPPORTS_MACCATALYST NO
            XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1,2")
    endif()
    add_custom_target(mkw_release DEPENDS WiiCompiled RetroRewind)
else()
    add_custom_target(mkw_release DEPENDS WiiCompiled)
    message(STATUS "RetroRewind target disabled (run translate-mod and emit-build-shards)")
endif()

if(MKW_HAVE_RETRO_REWIND)
    if(ANDROID)
        add_library(KartPadDual SHARED
            "${MKW_KARTPAD_DUAL_PRODUCT_SOURCE}"
            ${MKW_BASE_REGISTRATION_SOURCES}
            ${MKW_RETRO_REGISTRATION_SOURCES})
        set_target_properties(KartPadDual PROPERTIES OUTPUT_NAME main)
    else()
        add_executable(KartPadDual
            "${MKW_KARTPAD_DUAL_PRODUCT_SOURCE}"
            ${MKW_BASE_REGISTRATION_SOURCES}
            ${MKW_RETRO_REGISTRATION_SOURCES})
    endif()
    mkw_configure_product(KartPadDual)
    if(ANDROID)
        target_precompile_headers(KartPadDual PRIVATE
            "${MKW_RUNTIME_SOURCE_DIR}/include/mkw_pch.h")
    else()
        target_precompile_headers(KartPadDual REUSE_FROM WiiCompiled)
    endif()
    if(TARGET mkw_base_sensitive)
        target_sources(KartPadDual PRIVATE $<TARGET_OBJECTS:mkw_base_sensitive>)
    endif()
    if(TARGET mkw_retro_sensitive)
        target_sources(KartPadDual PRIVATE $<TARGET_OBJECTS:mkw_retro_sensitive>)
    endif()
    target_sources(KartPadDual PRIVATE $<TARGET_OBJECTS:mkw_retro_rewind_functions>)
    if(MKW_RETRO_BLOB_OBJECTS)
        target_sources(KartPadDual PRIVATE ${MKW_RETRO_BLOB_OBJECTS})
    endif()
    if(APPLE AND NOT CMAKE_SYSTEM_NAME STREQUAL "iOS" AND
       DEFINED MKW_KARTPAD_MACOS_SHELL AND MKW_KARTPAD_MACOS_SHELL)
        target_sources(KartPadDual PRIVATE ${MKW_KARTPAD_MACOS_SOURCES})
        target_include_directories(KartPadDual PRIVATE
            "${MKW_KARTPAD_MACOS_DIR}" "${MKW_KARTPAD_REPO_ROOT}/apple/shared"
            "${MKW_KARTPAD_REPO_ROOT}/runtime/include"
            "${CMAKE_CURRENT_LIST_DIR}/../third_party/kartpad-profile")
        target_compile_definitions(KartPadDual PRIVATE KARTPAD_RUNTIME_PRODUCT_DUAL=1)
        target_link_libraries(KartPadDual PRIVATE
            "-framework AppKit" "-framework UniformTypeIdentifiers"
            "-framework IOBluetooth" "-framework IOKit")
    endif()
    if(CMAKE_SYSTEM_NAME STREQUAL "iOS" AND
       DEFINED MKW_KARTPAD_MOBILE_SOURCES AND MKW_KARTPAD_MOBILE_SOURCES)
        set_target_properties(WiiCompiled PROPERTIES OUTPUT_NAME WiiCompiledBase)
        set_target_properties(RetroRewind PROPERTIES OUTPUT_NAME RetroRewindStandalone)
        target_sources(KartPadDual PRIVATE ${MKW_KARTPAD_MOBILE_SOURCES}
            "${MKW_KARTPAD_ICON_CATALOG}" "${MKW_KARTPAD_PRIVACY_MANIFEST}")
        target_include_directories(KartPadDual PRIVATE
            "${MKW_KARTPAD_IOS_DIR}"
            "${MKW_KARTPAD_MOBILE_DIR}"
            "${MKW_KARTPAD_SHARED_DIR}"
            "${MKW_KARTPAD_SUNPAD_DIR}"
            "${MKW_KARTPAD_REPO_ROOT}/runtime/include"
            "${MKW_KARTPAD_DISCIO_SOURCE_DIR}/Source/Core"
            "${MKW_KARTPAD_DISCIO_SOURCE_DIR}/Externals/fmt/fmt/include"
            "${MKW_KARTPAD_DISCIO_SOURCE_DIR}/Externals/minizip-ng/minizip-ng"
            "${CMAKE_CURRENT_LIST_DIR}/../third_party/kartpad-profile")
        target_link_options(KartPadDual PRIVATE "-ObjC")
        target_link_libraries(KartPadDual PRIVATE
            ${MKW_KARTPAD_DISCIO_ARCHIVES}
            "-framework UIKit" "-framework CoreGraphics" "-framework QuartzCore"
            "-framework Metal" "-framework GameController"
            "-framework CoreMotion" "-framework UniformTypeIdentifiers"
            "-framework SystemConfiguration" "-framework CoreFoundation"
            "-framework CoreServices" "-framework Foundation" "-framework SafariServices"
            "-lcompression" "-lresolv")
        set_target_properties(KartPadDual PROPERTIES
            OUTPUT_NAME KartPad
            MACOSX_BUNDLE TRUE
            MACOSX_BUNDLE_INFO_PLIST "${MKW_KARTPAD_IOS_DIR}/RuntimeInfo.plist"
            XCODE_ATTRIBUTE_ARCHS arm64
            XCODE_ATTRIBUTE_ASSETCATALOG_COMPILER_APPICON_NAME AppIcon
            XCODE_ATTRIBUTE_CLANG_CXX_LANGUAGE_STANDARD "c++20"
            XCODE_ATTRIBUTE_CURRENT_PROJECT_VERSION 43
            XCODE_ATTRIBUTE_GENERATE_INFOPLIST_FILE NO
            XCODE_ATTRIBUTE_IPHONEOS_DEPLOYMENT_TARGET 16.0
            XCODE_ATTRIBUTE_MARKETING_VERSION 0.4.21
            XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER dev.kartpad.app
            XCODE_ATTRIBUTE_SUPPORTED_PLATFORMS "iphonesimulator iphoneos"
            XCODE_ATTRIBUTE_SUPPORTS_MACCATALYST NO
            XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1,2")
    endif()
    add_dependencies(mkw_release KartPadDual)
endif()

set(MKW_ALL_BUILD_TARGETS
    mkw_runtime_common mkw_base_shared mkw_base_sensitive mkw_retro_sensitive
    mkw_retro_rewind_functions WiiCompiled RetroRewind KartPadDual)
foreach(target IN LISTS MKW_ALL_BUILD_TARGETS)
    if(TARGET ${target})
        # Physical iOS includes older arm64 devices without FEAT_LRCPC.
        # Keep the existing macOS and Simulator target unchanged.
        if(CMAKE_SYSTEM_NAME STREQUAL "tvOS" OR
           (CMAKE_SYSTEM_NAME STREQUAL "iOS" AND
            CMAKE_OSX_SYSROOT MATCHES "iphoneos|iPhoneOS"))
            target_compile_options(${target} PRIVATE
                "SHELL:-mcpu=generic -Xclang -target-feature -Xclang -rcpc")
        else()
            target_compile_options(${target} PRIVATE -mcpu=apple-m2)
        endif()
    endif()
endforeach()
