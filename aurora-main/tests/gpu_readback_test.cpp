// ROM-free integration probe. Links the actual maintained Aurora renderer.
#include "gfx/common.hpp"
#include "gfx/clear.hpp"
#include "gfx/efb_ram_copy.hpp"
#include "gfx/pipeline_cache.hpp"
#include "gfx/texture.hpp"
#include "gfx/kartpad_selfcheck.hpp"
#include "gx/gx.hpp"
#include "gx/fifo.hpp"
#include "gx/command_processor.hpp"
#include "gx/frame_interpolation.hpp"
#include <dolphin/gx.h>
#include <aurora/aurora.h>
#include <array>
#include <bit>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <thread>
#include <vector>

// The standalone host probe does not link the Android app tracing service.
#if !defined(__ANDROID__)
extern "C" bool KartPadAndroidBeginTrace(const char*) { return false; }
extern "C" void KartPadAndroidEndTrace() {}
#endif

namespace {
using namespace aurora;
std::atomic<unsigned> errors{};
std::atomic<bool> selfcheckMatched{};
std::atomic<unsigned> guestWrites{};
// Keep destinations alive through shutdown, including any failing wait.
std::array<uint8_t, 16 * 16 * 4 + 32> guarded;
std::array<uint8_t, 16 * 16 * 4 + 32> guardedBake;
void require(bool value, const char* message) {
  if (!value) throw std::runtime_error(message);
}
void submit(bool final, bool download = false, bool async = false) {
  auto encoder = webgpu::g_device.CreateCommandEncoder();
  if (final) gfx::end_frame(encoder); else gfx::end_batch(encoder);
  gfx::render(encoder);
  if (download) gfx::efb_ram::encode_downloads(encoder);
  if (async) gfx::efb_ram::encode_async_downloads(encoder);
  auto commands = encoder.Finish();
  webgpu::g_queue.Submit(1, &commands);
  if (download) require(gfx::efb_ram::complete_downloads(), "EFB readback failed");
  gfx::after_submit();
  if (!final) require(gfx::resume_frame(), "Batch resume failed");
}

constexpr std::array<std::array<uint8_t, 4>, 4> colors{{
    {255, 0, 0, 255}, {0, 255, 0, 255}, {0, 0, 255, 255}, {255, 255, 0, 255}}};
using Pixels = std::vector<uint8_t>;
Pixels expected(unsigned extent) {
  Pixels bytes(extent * extent * 4);
  // GX RGBA8: 4x4 tiles, sixteen A/R pairs followed by sixteen G/B pairs.
  for (unsigned y = 0; y < extent; ++y) for (unsigned x = 0; x < extent; ++x) {
    const auto color = colors[y * 4 / extent];
    const auto tile = ((y / 4) * (extent / 4) + x / 4) * 64;
    const auto pair = ((y % 4) * 4 + x % 4) * 2;
    bytes[tile + pair] = color[3]; bytes[tile + pair + 1] = color[0];
    bytes[tile + 32 + pair] = color[1]; bytes[tile + 33 + pair] = color[2];
  }
  return bytes;
}

Pixels run(unsigned splitEvery, bool async = false, bool offscreen = false, unsigned geometry = 0, bool capacityStress = false, bool interpolate = false, bool frameWorker = false, unsigned copyCase = 0, unsigned matrixUpperBits = 0) {
  require(!async || !offscreen, "Combined probe mode is not supported");
  guardedBake.fill(0xa5);
  gx::g_gxState.clearColor = {0.f, 0.f, 0.f, 1.f};
  require(frameWorker ? aurora_begin_frame() : gfx::begin_frame(), "Frame begin failed");
  std::array<std::array<float, 3>, 4> positions{};
  alignas(32) std::array<uint8_t, 64> matrixTexture{};
  GXTexObj matrixTextureObject{};
  // Synthetic character-format probes: direct F32, indexed S16/S8 with
  // RGBA8/RGB565 colors, and the 7/10-byte indexed formats seen in diagnostics.
  const bool normalTexgens = geometry == 9 || geometry == 10;
  const bool skinned = geometry >= 6 && geometry <= 10;
  std::array<uint8_t, 24> indexedPositions{};
  std::array<int8_t, 12> indexedNormals{};
  std::array<uint8_t, 48> indexedFloatPositions{};
  std::array<uint8_t, 24> indexedS16Normals{};
  const std::array<uint8_t, 4> indexedUV{uint8_t(geometry == 10 ? 0x40 : 0x20), 0x00, uint8_t(geometry == 10 ? 0x60 : 0x30), 0x00};
  const std::array<uint8_t, 8> indexedColors565{0xf8, 0, 0x07, 0xe0, 0, 0x1f, 0xff, 0xe0};
  if (geometry) {
    alignas(32) static std::array<uint8_t, 32768> fifo;
    GXInit(fifo.data(), fifo.size());
    gx::g_gxState.viewportPolicy = AURORA_VIEWPORT_NATIVE;
    GXSetViewport(0.f, 0.f, 64.f, 64.f, 0.f, 1.f);
    GXSetScissor(0, 0, 64, 64);
    GXSetCullMode(GX_CULL_NONE);
    GXSetZMode(false, GX_ALWAYS, false);
    GXSetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_COPY);
    GXSetColorUpdate(true); GXSetAlphaUpdate(true);
    GXSetNumTexGens(0); GXSetNumChans(1); GXSetNumTevStages(copyCase ? copyCase : 1);
    for (unsigned stage = 1; stage < copyCase; ++stage) {
      GXSetTevOrder(static_cast<GXTevStageID>(stage), GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR0A0);
      GXSetTevOp(static_cast<GXTevStageID>(stage), GX_PASSCLR);
    }
    GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR0A0);
    GXSetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
    GXSetChanCtrl(GX_COLOR0A0, false, GX_SRC_REG, GX_SRC_VTX, GX_LIGHT_NULL, GX_DF_NONE, GX_AF_NONE);
    const float projection[]{interpolate ? 0.f : 1.f, 1.f, 0.f, 1.f, 0.f, 0.f, -0.5f};
    GXSetProjectionv(projection);
    GXClearVtxDesc();
    GXSetVtxDesc(GX_VA_POS, geometry == 2 ? GX_INDEX8 : GX_DIRECT);
    GXSetVtxDesc(GX_VA_CLR0, GX_DIRECT);
    if (geometry == 2) GXSetArray(GX_VA_POS, positions.data(), sizeof(positions), sizeof(positions[0]), true);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
    if (geometry == 4 || geometry == 5) {
      const float identity[3][4]{{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}};
      GXLoadPosMtxImm(identity, GX_PNMTX1);
      GXSetVtxDesc(GX_VA_PNMTXIDX, GX_DIRECT);
    }
    if (geometry == 5) {
      // Upper texture rows are black and lower rows white. Only TEXMTX0's
      // translation selects white, preserving the four independently expected colors.
      for (unsigned pixel = 0; pixel < 16; ++pixel) {
        const uint8_t c = pixel >= 8 ? 255 : 0;
        matrixTexture[pixel * 2] = 255;
        matrixTexture[pixel * 2 + 1] = c;
        matrixTexture[32 + pixel * 2] = c;
        matrixTexture[33 + pixel * 2] = c;
      }
      GXInitTexObj(&matrixTextureObject, matrixTexture.data(), 4, 4, GX_TF_RGBA8,
                   GX_CLAMP, GX_CLAMP, GX_FALSE);
      GXInitTexObjLOD(&matrixTextureObject, GX_NEAR, GX_NEAR, 0, 0, 0, GX_FALSE, GX_FALSE, GX_ANISO_1);
      GXLoadTexObj(&matrixTextureObject, GX_TEXMAP0);
      const float translated[3][4]{{1, 0, 0, 0.5f}, {0, 1, 0, 0.75f}, {0, 0, 1, 0}};
      GXLoadTexMtxImm(translated, GX_TEXMTX0, GX_MTX2x4);
      GXSetNumTexGens(1);
      GXSetTexCoordGen2(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_TEXMTX0, GX_FALSE, GX_PTIDENTITY);
      GXSetVtxDesc(GX_VA_TEX0MTXIDX, GX_DIRECT);
      GXSetVtxDesc(GX_VA_TEX0, GX_DIRECT);
      GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);
      GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
      GXSetTevOp(GX_TEVSTAGE0, GX_MODULATE);
    }
    if (skinned) {
      GXSetZMode(GX_TRUE, GX_LESS, GX_TRUE);
      GXSetVtxDesc(GX_VA_PNMTXIDX, GX_DIRECT);
      GXSetVtxDesc(GX_VA_NRM, geometry == 6 ? GX_DIRECT : geometry == 7 ? GX_INDEX8 : GX_INDEX16);
      GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_NRM, GX_NRM_XYZ, geometry == 6 ? GX_F32 : GX_S8, 0);
      for (unsigned slot = 0; slot < 10; ++slot) {
        const float position[3][4]{{1,0,0,slot/32.f}, {0,1,0,0}, {0,0,1,0}};
        const float normal[3][4]{{1,0,0,0}, {0,1,0,0}, {0,0,slot%2 ? -1.f : 1.f,0}};
        GXLoadPosMtxImm(position, slot * 3);
        GXLoadNrmMtxImm(normal, slot * 3);
      }
      GXLightObj light{};
      GXInitLightPos(&light, 0, 0, 1.e8f);
      GXInitLightColor(&light, {255,255,255,255});
      GXInitLightAttn(&light, 1,0,0,1,0,0);
      GXLoadLightObjImm(&light, GX_LIGHT0);
      GXSetChanAmbColor(GX_COLOR0A0, {0,0,0,0});
      GXSetChanCtrl(GX_COLOR0, GX_TRUE, GX_SRC_REG, GX_SRC_VTX, GX_LIGHT0, GX_DF_CLAMP, GX_AF_NONE);
      GXSetChanCtrl(GX_ALPHA0, GX_FALSE, GX_SRC_REG, GX_SRC_VTX, GX_LIGHT_NULL, GX_DF_NONE, GX_AF_NONE);
      if (geometry != 6) {
        GXSetVtxDesc(GX_VA_POS, geometry == 7 ? GX_INDEX8 : GX_INDEX16);
        GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_S16, 8);
        GXSetArray(GX_VA_POS, indexedPositions.data(), indexedPositions.size(), 6, false);
        GXSetArray(GX_VA_NRM, indexedNormals.data(), indexedNormals.size(), 3, false);
        GXSetVtxDesc(GX_VA_CLR0, geometry == 7 ? GX_INDEX8 : GX_INDEX16);
        GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, geometry == 7 ? GX_CLR_RGBA : GX_CLR_RGB,
                       geometry == 7 ? GX_RGBA8 : GX_RGB565, 0);
        if (geometry == 7) GXSetArray(GX_VA_CLR0, colors.data(), sizeof(colors), 4, false);
        else GXSetArray(GX_VA_CLR0, indexedColors565.data(), indexedColors565.size(), 2, false);
      }
    }
    if (normalTexgens) {
      // Reported layouts: PN, TEX1, TEX2, POS8/16, NRM8/16, CLR8, TEX8/16.
      GXSetZMode(GX_TRUE, GX_LEQUAL, GX_TRUE);
      GXSetChanCtrl(GX_COLOR0, GX_TRUE, GX_SRC_REG, GX_SRC_VTX, GX_LIGHT0, GX_DF_CLAMP, GX_AF_SPOT);
      if (geometry == 10)
        GXSetChanCtrl(GX_ALPHA0, GX_TRUE, GX_SRC_REG, GX_SRC_VTX, GX_LIGHT0, GX_DF_CLAMP, GX_AF_SPOT);
      GXSetVtxDesc(GX_VA_TEX1MTXIDX, GX_DIRECT);
      GXSetVtxDesc(GX_VA_TEX2MTXIDX, GX_DIRECT);
      GXSetVtxDesc(GX_VA_POS, geometry == 10 ? GX_INDEX16 : GX_INDEX8);
      GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
      GXSetArray(GX_VA_POS, indexedFloatPositions.data(), indexedFloatPositions.size(), 12, false);
      GXSetVtxDesc(GX_VA_NRM, geometry == 10 ? GX_INDEX16 : GX_INDEX8);
      GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_NRM, GX_NRM_XYZ, GX_S16, 0);
      GXSetArray(GX_VA_NRM, indexedS16Normals.data(), indexedS16Normals.size(), 6, false);
      GXSetVtxDesc(GX_VA_CLR0, GX_INDEX8);
      GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
      GXSetArray(GX_VA_CLR0, colors.data(), sizeof(colors), 4, false);
      GXSetVtxDesc(GX_VA_TEX0, geometry == 10 ? GX_INDEX16 : GX_INDEX8);
      GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST, geometry == 10 ? GX_U16 : GX_S16, geometry == 10 ? 15 : 14);
      GXSetArray(GX_VA_TEX0, indexedUV.data(), indexedUV.size(), 4, false);
      for (unsigned pixel = 0; pixel < 16; ++pixel) {
        const uint8_t c = pixel >= 8 ? 255 : 0;
        matrixTexture[pixel * 2] = 255;
        matrixTexture[pixel * 2 + 1] = c;
        matrixTexture[32 + pixel * 2] = c;
        matrixTexture[33 + pixel * 2] = c;
      }
      GXInitTexObj(&matrixTextureObject, matrixTexture.data(), 4, 4, GX_TF_RGBA8,
                   GX_CLAMP, GX_CLAMP, GX_FALSE);
      GXInitTexObjLOD(&matrixTextureObject, GX_NEAR, GX_NEAR, 0, 0, 0, GX_FALSE, GX_FALSE, GX_ANISO_1);
      for (unsigned slot = 0; slot < 10; ++slot) {
        const float transform[3][4]{{1,0,0,0}, {0,1,0,0}, {0,0,slot%2 ? -2.f : 2.f,0}};
        GXLoadTexMtxImm(transform, GX_TEXMTX0 + slot * 3, GX_MTX3x4);
      }
      const float identity[3][4]{{1,0,0,0}, {0,1,0,0}, {0,0,1,0}};
      const float post[3][4]{{0,0,0,0.5f}, {0,0,-0.375f,1.125f}, {0,0,0,1}};
      GXLoadTexMtxImm(identity, GX_PTTEXMTX0, GX_MTX3x4);
      GXLoadTexMtxImm(post, GX_PTTEXMTX1, GX_MTX3x4);
      GXLoadTexMtxImm(post, GX_PTTEXMTX2, GX_MTX3x4);
      GXSetNumTexGens(3);
      GXSetTexCoordGen2(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY, GX_FALSE, GX_PTTEXMTX0);
      GXSetTexCoordGen2(GX_TEXCOORD1, GX_TG_MTX3x4, GX_TG_NRM, GX_IDENTITY, GX_TRUE, GX_PTTEXMTX1);
      GXSetTexCoordGen2(GX_TEXCOORD2, GX_TG_MTX3x4, GX_TG_NRM, GX_IDENTITY, GX_TRUE, GX_PTTEXMTX2);
      GXSetNumTevStages(3);
      for (unsigned stage = 0; stage < 3; ++stage) {
        const auto id = static_cast<GXTevStageID>(stage);
        GXLoadTexObj(&matrixTextureObject, static_cast<GXTexMapID>(stage));
        GXSetTevOrder(id, static_cast<GXTexCoordID>(stage), static_cast<GXTexMapID>(stage), GX_COLOR0A0);
        GXSetTevColorIn(id, GX_CC_ZERO, GX_CC_TEXC, stage ? GX_CC_CPREV : GX_CC_RASC, GX_CC_ZERO);
        GXSetTevColorOp(id, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
        GXSetTevAlphaIn(id, GX_CA_ZERO, GX_CA_TEXA, stage ? GX_CA_APREV : GX_CA_RASA, GX_CA_ZERO);
        GXSetTevAlphaOp(id, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
      }
      GXSetAlphaCompare(GX_GEQUAL, 128, GX_AOP_AND, GX_LEQUAL, 255);
    }
    gx::fifo::drain();

  }
  const auto pipeline = gfx::pipeline_ref(gfx::clear::PipelineConfig{});
  for (unsigned band = 0; band < 4; ++band) {
    const auto c = colors[band];
    if (geometry) {
      const float top = 1.f - band * 0.5f;
      const float bottom = top - 0.5f;
      const float z = interpolate ? -1.f : 0.f;
      positions = {{{-1.f, top, z}, {1.f, top, z}, {1.f, bottom, z}, {-1.f, bottom, z}}};
      if (skinned) {
        for (unsigned index = 0; index < 4; ++index) {
          const unsigned slot = (band * 3 + index) % 10;
          positions[index][0] -= slot / 32.f;
          for (unsigned axis = 0; axis < 3; ++axis) {
            const auto raw = static_cast<uint16_t>(static_cast<int16_t>(positions[index][axis] * 256));
            indexedPositions[index * 6 + axis * 2] = raw >> 8;
            indexedPositions[index * 6 + axis * 2 + 1] = raw;
          }
          indexedNormals[index * 3 + 2] = slot % 2 ? -64 : 64;
          for (unsigned axis = 0; axis < 3; ++axis) {
            const auto bits = std::bit_cast<uint32_t>(positions[index][axis]);
            for (unsigned byte = 0; byte < 4; ++byte)
              indexedFloatPositions[index * 12 + axis * 4 + byte] = bits >> (24 - byte * 8);
          }
          const auto normal = static_cast<uint16_t>(static_cast<int16_t>(slot % 2 ? -16384 : 16384));
          indexedS16Normals[index * 6 + 4] = normal >> 8;
          indexedS16Normals[index * 6 + 5] = normal;
        }
        if (geometry != 6) GXInvalidateVtxCache();
      }
      // Keep the same array address/format and change only its bytes between draws.
      if (geometry == 2) GXInvalidateVtxCache();
      if (geometry == 3) {
        std::array<uint8_t, 64> raw{};
        for (unsigned index = 0; index < positions.size(); ++index) {
          for (unsigned axis = 0; axis < 3; ++axis) {
            const auto bits = std::bit_cast<uint32_t>(positions[index][axis]);
            for (unsigned byte = 0; byte < 4; ++byte)
              raw[index * 16 + axis * 4 + byte] = bits >> (24 - byte * 8);
          }
          std::copy(c.begin(), c.end(), raw.begin() + index * 16 + 12);
        }
        require(gx::fifo::submit_raw_draw(GX_QUADS, GX_VTXFMT0, raw.data(), 4, raw.size()),
                "Raw bridge rejected valid quad");
      } else {
      GXBegin(GX_QUADS, GX_VTXFMT0, 4);
      for (unsigned index = 0; index < positions.size(); ++index) {
        if (geometry == 4 || geometry == 5)
          GXMatrixIndex1u8(GX_VA_PNMTXIDX, GX_PNMTX1 | (geometry == 4 ? matrixUpperBits : 0));
        if (geometry == 5) GXMatrixIndex1u8(GX_VA_TEX0MTXIDX, GX_TEXMTX0 | matrixUpperBits);
        if (skinned) GXMatrixIndex1u8(GX_VA_PNMTXIDX, ((band * 3 + index) % 10) * 3);
        if (normalTexgens) {
          const unsigned slot = (band * 3 + index) % 10;
          GXMatrixIndex1u8(GX_VA_TEX1MTXIDX, GX_TEXMTX0 + slot * 3);
          GXMatrixIndex1u8(GX_VA_TEX2MTXIDX, GX_TEXMTX0 + ((slot + 2) % 10) * 3);
        }
        if (geometry == 2 || geometry == 7 || geometry == 9) GXPosition1x8(index);
        else if (geometry == 8 || geometry == 10) GXPosition1x16(index);
        else GXPosition3f32(positions[index][0], positions[index][1], positions[index][2]);
        if (geometry == 6) GXNormal3f32(0, 0, ((band * 3 + index) % 10) % 2 ? -1.f : 1.f);
        else if (geometry == 7 || geometry == 9) GXNormal1x8(index);
        else if (geometry == 8 || geometry == 10) GXNormal1x16(index);
        if (geometry == 7 || normalTexgens) GXColor1x8(band);
        else if (geometry == 8) GXColor1x16(band);
        else GXColor4u8(c[0], c[1], c[2], 255);
        if (geometry == 5) GXTexCoord2f32(0.f, 0.f);
        if (geometry == 9) GXTexCoord1x8(0);
        else if (geometry == 10) GXTexCoord1x16(0);
      }
      GXEnd();
    if (normalTexgens && band == 0) {
      gx::PipelineConfig probe{};
      gx::populate_pipeline_config(probe, GX_QUADS, GX_VTXFMT0);
      const auto info = gx::build_shader_info(probe.shaderConfig);
      require(probe.shaderConfig.vtxStride == (geometry == 9 ? 7 : 10), "Character probe vertex layout drifted");
      require(info.matrixLayout.postexCount == 20 && info.matrixLayout.nrmCount == 10,
              "Character probe must exercise the full position/texture/normal palettes");
    }
      if (geometry == 6 && band == 0) {
        // Bypass only the diagnostic eligibility timer. Three identical LESS
        // depth-writing draws must receive independent clear/depth state.
        const auto* draw = gfx::get_last_draw_command<gx::DrawData>();
        require(draw != nullptr, "Missing draw for self-check isolation probe");
        const auto copy = *draw;
        gfx::kartpad_selfcheck::record(copy, copy, &copy, false, 4);
      }
      }
    } else {
    gfx::push_draw_command(gfx::clear::DrawData{
        .pipeline = pipeline,
        .color = {c[0] / 255., c[1] / 255., c[2] / 255., 1.},
        .depth = 0.5f,
        .useScissor = true,
        .scissor = {0, static_cast<int32_t>(band * 16), 64, 16}});
    }
    if (offscreen && band == 0) {
      // Suspend a partially recorded EFB, bake an independently observable copy,
      // then resume it before a possible capacity-boundary submission.
      gfx::begin_offscreen(64, 64);
      gfx::push_draw_command(gfx::clear::DrawData{
          .pipeline = pipeline, .color = {1., 0., 1., 1.}, .depth = 0.25f});
      if (capacityStress) for (unsigned draw = 0; draw < 24; ++draw) {
        gfx::push_draw_command(gfx::clear::DrawData{
            .pipeline = pipeline, .color = {1., 0., 1., 1.}, .depth = 0.25f,
            .useScissor = true, .scissor = {0, 0, 4, 4}});
      }
      auto baked = gfx::new_render_texture(64, 64, GX_TF_RGBA8, "Aurora probe offscreen bake");
      gfx::resolve_pass(baked, {0, 0, 64, 64}, false, false, false,
                        {0.f, 0.f, 0.f, 1.f}, 1.f, GX_TF_RGBA8, nullptr, false,
                        nullptr, false, 1.f, false, false, true);
      gfx::efb_ram::schedule(guardedBake.data() + 16, 16, 16, GX_TF_RGBA8, baked);
      gfx::end_offscreen();
      if (capacityStress) {
        require(gfx::efb_ram::prepare_downloads(), "Early bake readback preparation failed");
        for (unsigned draw = 0; draw < 24; ++draw) {
          gfx::push_draw_command(gfx::clear::DrawData{
              .pipeline = pipeline, .color = {1., 0., 0., 1.}, .depth = 0.5f,
              .useScissor = true, .scissor = {0, 0, 4, 4}});
        }
      }
    }
    if (splitEvery && band < 3 && (band + 1) % splitEvery == 0) submit(false);
  }
  auto texture = gfx::new_render_texture(64, 64, GX_TF_RGBA8, "Aurora probe persistent copy");
  static std::array<uint8_t, 64 * 64 * 4> copyDestination;
  if (copyCase) {
    gx::fifo::drain();
    GXSetTexCopySrc(0, 0, 64, 64);
    GXSetTexCopyDst(64, 64, GX_TF_RGBA8, GX_FALSE);
    GXCopyTex(copyDestination.data(), GX_FALSE);
    texture = gx::g_gxState.copyTextures.at(copyDestination.data()).handle;
  } else {
  // A partial clear forces the real snapshot and clear-uniform paths after the copy.
  gfx::resolve_pass(texture, {0, 0, 64, 64}, true, true, true, {0.f, 0.f, 0.f, 1.f},
                    1.f, GX_TF_RGBA8, nullptr, false, nullptr, false, 1.f, false, false, true);
  }
  guarded.fill(0xa5);
  const unsigned extent = async ? 4 : 16;
  const unsigned bytes = extent * extent * 4;
  gfx::efb_ram::schedule(guarded.data() + 16, extent, extent, GX_TF_RGBA8, texture);
  const auto before = guestWrites.load(std::memory_order_acquire);
  if (async) gfx::efb_ram::seal_async_downloads();
  else require(gfx::efb_ram::prepare_downloads(), "Readback preparation failed");
  if (frameWorker) {
    require(!async && aurora_flush_efb_copies_to_ram(), "Worker-mode EFB readback failed");
    aurora_end_frame();
  } else submit(true, !async, async);
  if (async) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (guestWrites.load(std::memory_order_acquire) == before) {
      webgpu::g_instance.ProcessEvents();
      require(std::chrono::steady_clock::now() < deadline, "Async readback did not complete");
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }
  require(std::all_of(guarded.begin(), guarded.begin() + 16, [](auto b) { return b == 0xa5; }) &&
          std::all_of(guarded.begin() + 16 + bytes, guarded.end(), [](auto b) { return b == 0xa5; }),
          "Readback wrote outside its destination");
  if (offscreen) {
    require(std::all_of(guardedBake.begin(), guardedBake.begin() + 16, [](auto b) { return b == 0xa5; }) &&
            std::all_of(guardedBake.end() - 16, guardedBake.end(), [](auto b) { return b == 0xa5; }),
            "Offscreen readback wrote outside its destination");
    for (unsigned tile = 0; tile < 16; ++tile) for (unsigned pair = 0; pair < 16; ++pair) {
      const auto offset = 16 + tile * 64 + pair * 2;
      require(guardedBake[offset] == 255 && guardedBake[offset + 1] == 255 &&
              guardedBake[offset + 32] == 0 && guardedBake[offset + 33] == 255,
              "Offscreen bake did not preserve expected magenta pixels");
    }
  }
  Pixels pixels(guarded.begin() + 16, guarded.begin() + 16 + bytes);
  return pixels;
}
} // namespace

int main(int argc, char** argv) {
  if (argc != 2) return 2;
  std::filesystem::create_directories(argv[1]);
  AuroraConfig config{};
  config.appName = "Aurora readback regression tests";
  config.userPath = argv[1];
  config.cachePath = argv[1];
  config.resourcesPath = argv[1];
  config.desiredBackend = BACKEND_AUTO;
  config.windowWidth = 64;
  config.windowHeight = 64;
  config.msaa = 1;
  config.maxTextureAnisotropy = 1;
  config.logLevel = LOG_INFO;
  config.logCallback = [](AuroraLogLevel level, const char* module, const char* message, unsigned size) {
    if (level >= LOG_ERROR) ++errors;
    if (std::string_view(module) == "aurora::gfx::selfcheck" &&
        std::string_view(message, size).find("result=match") != std::string_view::npos)
      selfcheckMatched = true;
    std::fprintf(stderr, "[%s] %.*s\n", module, static_cast<int>(size), message);
  };
  const auto initialized = aurora_initialize(1, argv, &config);
  if (initialized.initializationStatus != AURORA_INITIALIZATION_SUCCESS) return 3;
  aurora_set_skip_unready_pipelines(true);
  aurora_set_guest_write_hooks(nullptr, [](const void*, size_t) {
    guestWrites.fetch_add(1, std::memory_order_release);
  });
  try {
    const auto prewarmQueued = gfx::queued_pipeline_count();
    const auto prewarmDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    while (gfx::queued_pipeline_count() != 0) {
      require(std::chrono::steady_clock::now() < prewarmDeadline, "Seeded pipeline prewarm did not finish");
      webgpu::g_instance.ProcessEvents();
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    std::printf("Actual Aurora completed seeded startup queue (%u observed pending)\n", prewarmQueued);
    // GX matrix selectors are six bits (Dolphin VertexLoader's direct-byte path).
    // Exercise real FIFO decode, generated WGSL and GPU output. The same binary
    // also runs with KARTPAD_RENDERER_VERTEX_REPACK=2 and VERTEX_PNMTX=1.
    bool matrixSelectorsMatched = true;
    for (unsigned geometry : {4u, 5u}) {
      for (unsigned bits : {0u, 0x40u, 0x80u, 0xc0u}) {
        const bool matched = run(0, false, false, geometry, false, false, false, 0, bits) == expected(16);
        matrixSelectorsMatched &= matched;
        std::printf("Actual Aurora matrix selector geometry=%u upper_bits=%u matched=%d\n", geometry, bits, matched);
      }
    }
    require(matrixSelectorsMatched, "Matrix selector upper bits changed pixels");
    for (unsigned geometry : {6u, 7u, 8u, 9u, 10u}) {
      const auto pixels = run(0, false, false, geometry);
      for (unsigned band = 0; band < 4; ++band) {
        const auto tile = band * 4 * 64;
        std::printf("Character probe geometry=%u band=%u ARGB=%u,%u,%u,%u\n", geometry, band,
                    pixels[tile], pixels[tile+1], pixels[tile+32], pixels[tile+33]);
      }
      require(pixels == expected(16), "Skinned palette/lighting/indexed-format probe changed pixels");
      std::printf("Actual Aurora skinned probe geometry=%u matched expected pixels\n", geometry);
    }
    const auto selfcheckDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!selfcheckMatched) {
      webgpu::g_instance.ProcessEvents();
      require(std::chrono::steady_clock::now() < selfcheckDeadline, "Identical LESS-depth self-check copies did not match");
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    std::printf("Actual self-check independently rendered three identical LESS-depth copies\n");
    const auto control = run(0);
    for (unsigned band = 0; band < 4; ++band) {
      const auto tile = band * 4 * 64;
      std::fprintf(stderr, "band=%u ARGB=%u,%u,%u,%u\n", band, control[tile],
                   control[tile + 1], control[tile + 32], control[tile + 33]);
    }
    require(control == expected(16), "Unsplit pixels differ from independently expected GX data");
    for (unsigned iteration = 0; iteration < 9; ++iteration) {
      const auto splitEvery = iteration % 3 + 1;
      require(run(splitEvery) == control, "Split pixels differ from unsplit control");
      std::printf("Actual Aurora split=%u iteration=%u matched native tiled readback\n", splitEvery, iteration);
    }
    for (unsigned iteration = 0; iteration < 9; ++iteration) {
      require(run(iteration % 3 + 1, true) == expected(4), "Async pixels differ from expected GX data");
      std::printf("Actual Aurora async iteration=%u matched native tiled readback\n", iteration);
    }
    for (unsigned splitEvery = 0; splitEvery < 4; ++splitEvery) {
      require(run(splitEvery, false, true) == expected(16), "Offscreen interlude changed the suspended EFB");
      std::printf("Actual Aurora offscreen split=%u preserved bake and suspended EFB\n", splitEvery);
    }
    for (unsigned splitEvery = 0; splitEvery < 4; ++splitEvery) {
      require(run(splitEvery, false, false, true) == expected(16), "GX FIFO quad pixels differ from expected output");
      std::printf("Actual Aurora GX FIFO split=%u preserved direct vertices, indices and uniforms\n", splitEvery);
    }
    for (unsigned splitEvery = 0; splitEvery < 4; ++splitEvery) {
      require(run(splitEvery, false, false, 2) == expected(16), "Invalidated GX array pixels differ from expected output");
      std::printf("Actual Aurora GX invalidation split=%u refreshed the same array address\n", splitEvery);
    }
    const gfx::StagingSizes physical{gfx::VertexBufferSize, gfx::UniformBufferSize,
                                      gfx::IndexBufferSize, gfx::StorageBufferSize};
    const auto uniformTail = gx::MaxUniformSize + 32 * gfx::staging_uniform_bytes(48);
    for (unsigned buffer = 0; buffer < 4; ++buffer) {
      auto limits = physical;
      limits[buffer] = buffer == 0 ? 128 : buffer == 1 ? uniformTail + 512 :
                       buffer == 2 ? 24 : 2 * gfx::staging_storage_bytes(48);
      gfx::set_staging_capacity_limits_for_testing(limits);
      const auto before = gfx::staging_split_count();
      require(run(0, false, false, buffer == 1 ? 0 : buffer == 3 ? 2 : 1) == expected(16),
              "Automatic capacity split changed pixels");
      require(gfx::staging_split_count() > before, "Forced capacity did not split");
      const auto highWater = gfx::staging_high_water();
      for (unsigned i = 0; i < limits.size(); ++i)
        require(highWater[i] <= limits[i], "Actual staging usage exceeded admission budget");
      std::printf("Actual staging high-water V/U/I/S=%llu/%llu/%llu/%llu bytes\n",
          static_cast<unsigned long long>(highWater[0]), static_cast<unsigned long long>(highWater[1]),
          static_cast<unsigned long long>(highWater[2]), static_cast<unsigned long long>(highWater[3]));
      std::printf("Actual Aurora automatic capacity buffer=%u splits=%llu matched pixels\n", buffer,
                  static_cast<unsigned long long>(gfx::staging_split_count() - before));
    }
    auto limits = physical;
    limits[0] = 128;
    gfx::set_staging_capacity_limits_for_testing(limits);
    require(run(0, false, false, 3) == expected(16), "Raw bridge capacity split changed pixels");
    std::puts("Actual Aurora raw bridge capacity split preserved direct quad pixels");
    limits = physical;
    limits[1] = uniformTail + 768;
    gfx::set_staging_capacity_limits_for_testing(limits);
    const auto beforeBake = gfx::staging_split_count();
    require(run(0, false, true, 0, true) == expected(16),
            "Automatic offscreen split changed bake or suspended EFB");
    require(gfx::staging_split_count() - beforeBake >= 9, "Offscreen test did not reuse all staging slots");
    std::printf("Actual Aurora automatic offscreen/readback splits=%llu preserved all pixels\n",
                static_cast<unsigned long long>(gfx::staging_split_count() - beforeBake));
    gfx::set_staging_capacity_limits_for_testing(physical);
    aurora_set_frame_interpolation_fps(120);
    for (unsigned frame = 0; frame < 3; ++frame)
      require(run(0, false, false, 2, false, true) == expected(16), "Perspective warmup changed pixels");
    AuroraFrameInterpolationDiagnostics interpolation{};
    gx::get_frame_interpolation_diagnostics(interpolation);
    require(interpolation.matchable > 0 && interpolation.activeSamples > 0,
            "Interpolation probe did not establish matching perspective draws");
    limits = physical;
    limits[3] = 2 * gfx::staging_storage_bytes(48);
    gfx::set_staging_capacity_limits_for_testing(limits);
    require(run(0, false, false, 2, false, true) == expected(16), "Interpolated split changed native pixels");
    gx::get_frame_interpolation_diagnostics(interpolation);
    require(!interpolation.replaySafe, "Split frame incorrectly retained interpolation replay");
    aurora_set_frame_interpolation_fps(0);
    std::puts("Actual Aurora matched perspective interpolation survived capacity split and disabled replay");
    limits = physical;
    limits[0] = 32; // One quad needs 64 bytes: typed rejection before any draw allocation.
    gfx::set_staging_capacity_limits_for_testing(limits);
    bool oversized = false;
    try { run(0, false, false, 1); }
    catch (const gfx::StagingCapacityError&) { oversized = true; }
    require(oversized, "Oversized primitive was not rejected");
    require(gfx::staging_usage() == gfx::StagingSizes{}, "Oversized primitive partially allocated");
    gx::fifo::clear_buffer();
    gfx::abort_frame();
    gfx::set_staging_capacity_limits_for_testing(physical);
    std::puts("Actual Aurora oversized primitive rejected before staging mutation");
    require(run(0, false, false, 1) == expected(16), "Renderer failed after rejected primitive cleanup");
    limits = physical;
    limits[3] = 2 * gfx::staging_storage_bytes(48);
    gfx::set_staging_capacity_limits_for_testing(limits);
    aurora_set_frame_interpolation_fps(120);
    for (unsigned frame = 0; frame < 16; ++frame)
      require(run(0, false, false, 2, false, true, true) == expected(16),
              "Frame-worker capacity split changed pixels");
    // Grant preparation of the next frame before joining DONE, exactly as the
    // real producer does; leave no worker waiting for a future begin_frame.
    require(aurora_begin_frame(), "Final worker frame preparation failed");
    aurora::wait_for_frame_worker();
    gfx::abort_frame();
    aurora_set_frame_interpolation_fps(0);
    gfx::set_staging_capacity_limits_for_testing(physical);
    std::puts("Actual Aurora frame worker completed 16 capacity-split perspective frames");
    require(errors == 0, "Renderer reported an error");
  } catch (const std::exception& error) {
    std::fprintf(stderr, "FAIL: %s\n", error.what());
    aurora_shutdown();
    return 4;
  }
  aurora_shutdown();
  if (errors != 0) return 4;
  std::puts("Actual Aurora clear/resolve/snapshot/downsample/readback batches passed");
}
