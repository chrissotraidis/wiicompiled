if(AURORA_TARGET_RVL)
	set(AURORA_GX_TRANSFORM_SOURCE "${CMAKE_CURRENT_LIST_DIR}/../lib/revolution/gx/GXTransform.cpp")
else()
	set(AURORA_GX_TRANSFORM_SOURCE "${CMAKE_CURRENT_LIST_DIR}/../lib/dolphin/gx/GXTransform.cpp")
endif()

add_library(aurora_gx STATIC
        ${CMAKE_CURRENT_LIST_DIR}/../lib/gfx/clear.cpp
        ${CMAKE_CURRENT_LIST_DIR}/../lib/gfx/depth_peek.cpp
        ${CMAKE_CURRENT_LIST_DIR}/../lib/gfx/color_peek.cpp
        ${CMAKE_CURRENT_LIST_DIR}/../lib/gfx/encoding.cpp
        ${CMAKE_CURRENT_LIST_DIR}/../lib/gfx/frame.cpp
        ${CMAKE_CURRENT_LIST_DIR}/../lib/gfx/pipeline_cache.cpp
        ${CMAKE_CURRENT_LIST_DIR}/../lib/gfx/readback/copy_pack.cpp
        ${CMAKE_CURRENT_LIST_DIR}/../lib/gfx/readback/efb_gather.cpp
        ${CMAKE_CURRENT_LIST_DIR}/../lib/gfx/readback/gpu.cpp
        ${CMAKE_CURRENT_LIST_DIR}/../lib/gfx/readback/readback.cpp
        ${CMAKE_CURRENT_LIST_DIR}/../lib/gfx/recording.cpp
        ${CMAKE_CURRENT_LIST_DIR}/../lib/gfx/render_worker.cpp
        ${CMAKE_CURRENT_LIST_DIR}/../lib/gfx/resource_cache.cpp
        ${CMAKE_CURRENT_LIST_DIR}/../lib/gfx/dds_io.cpp
        ${CMAKE_CURRENT_LIST_DIR}/../lib/gfx/tex_copy_conv.cpp
        ${CMAKE_CURRENT_LIST_DIR}/../lib/gfx/tex_palette_conv.cpp
        ${CMAKE_CURRENT_LIST_DIR}/../lib/gfx/texture.cpp
        ${CMAKE_CURRENT_LIST_DIR}/../lib/gfx/texture_format.cpp
        ${CMAKE_CURRENT_LIST_DIR}/../lib/gfx/texture_convert.cpp
        ${CMAKE_CURRENT_LIST_DIR}/../lib/gfx/texture_replacement.cpp
        ${CMAKE_CURRENT_LIST_DIR}/../lib/gx/attr_fmt.cpp
        ${CMAKE_CURRENT_LIST_DIR}/../lib/gx/command_processor.cpp
        ${CMAKE_CURRENT_LIST_DIR}/../lib/gx/frame_interpolation.cpp
        ${CMAKE_CURRENT_LIST_DIR}/../lib/gx/regs.cpp
        ${CMAKE_CURRENT_LIST_DIR}/../lib/gx/dl.cpp
        ${CMAKE_CURRENT_LIST_DIR}/../lib/gx/fifo.cpp
        ${CMAKE_CURRENT_LIST_DIR}/../lib/gx/gx.cpp
        ${CMAKE_CURRENT_LIST_DIR}/../lib/gx/texture.cpp
        ${CMAKE_CURRENT_LIST_DIR}/../lib/gx/pipeline.cpp
        ${CMAKE_CURRENT_LIST_DIR}/../lib/gx/readback.cpp
        ${CMAKE_CURRENT_LIST_DIR}/../lib/gx/shader.cpp
        ${CMAKE_CURRENT_LIST_DIR}/../lib/gx/shader_info.cpp
        ${CMAKE_CURRENT_LIST_DIR}/../lib/dolphin/gx/GXBump.cpp
        ${CMAKE_CURRENT_LIST_DIR}/../lib/dolphin/gx/GXCull.cpp
        ${CMAKE_CURRENT_LIST_DIR}/../lib/dolphin/gx/GXCpu2Efb.cpp
        ${CMAKE_CURRENT_LIST_DIR}/../lib/dolphin/gx/GXDispList.cpp
        ${CMAKE_CURRENT_LIST_DIR}/../lib/dolphin/gx/GXDraw.cpp
        ${CMAKE_CURRENT_LIST_DIR}/../lib/dolphin/gx/GXExtra.cpp
        ${CMAKE_CURRENT_LIST_DIR}/../lib/dolphin/gx/GXFifo.cpp
        ${CMAKE_CURRENT_LIST_DIR}/../lib/dolphin/gx/GXFrameBuffer.cpp
        ${CMAKE_CURRENT_LIST_DIR}/../lib/dolphin/gx/GXGeometry.cpp
        ${CMAKE_CURRENT_LIST_DIR}/../lib/dolphin/gx/GXGet.cpp
        ${CMAKE_CURRENT_LIST_DIR}/../lib/dolphin/gx/GXLighting.cpp
        ${CMAKE_CURRENT_LIST_DIR}/../lib/dolphin/gx/GXManage.cpp
        ${CMAKE_CURRENT_LIST_DIR}/../lib/dolphin/gx/GXPerf.cpp
        ${CMAKE_CURRENT_LIST_DIR}/../lib/dolphin/gx/GXPixel.cpp
        ${CMAKE_CURRENT_LIST_DIR}/../lib/dolphin/gx/GXTev.cpp
        ${CMAKE_CURRENT_LIST_DIR}/../lib/dolphin/gx/GXTexture.cpp
        ${AURORA_GX_TRANSFORM_SOURCE}
        ${CMAKE_CURRENT_LIST_DIR}/../lib/dolphin/gx/GXVert.cpp
        ${CMAKE_CURRENT_LIST_DIR}/../lib/dolphin/gx/GXAurora.cpp
        ${CMAKE_CURRENT_LIST_DIR}/../lib/kartpad_compat.cpp
        ${CMAKE_CURRENT_LIST_DIR}/../lib/gfx/png_io.cpp
        ${CMAKE_CURRENT_LIST_DIR}/../lib/gfx/png_io.hpp
)
add_library(aurora::gx ALIAS aurora_gx)
set_target_properties(aurora_gx PROPERTIES FOLDER "aurora")

target_link_libraries(aurora_gx PUBLIC aurora::core dawn::webgpu_dawn xxHash::xxhash)
target_link_libraries(aurora_gx PRIVATE absl::btree absl::flat_hash_map sqlite3 Tracy::TracyClient PNG::PNG)
target_compile_definitions(aurora_gx PRIVATE WEBGPU_DAWN)

if (AURORA_ENABLE_RMLUI)
    target_sources(aurora_gx PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}/../lib/rmlui/pipeline.cpp
        ${CMAKE_CURRENT_LIST_DIR}/../lib/rmlui/pipeline.hpp
    )
endif ()

# Frame interpolation's NaN/Inf guards are load-bearing; never let finite-math assumptions fold them.
if (CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  set_source_files_properties(${CMAKE_CURRENT_LIST_DIR}/../lib/gx/frame_interpolation.cpp
      PROPERTIES COMPILE_OPTIONS "-fno-finite-math-only")
endif ()
