#include "gx.hpp"

#include "pipeline.hpp"
#include "texture.hpp"
#include "../dolphin/vi/vi_internal.hpp"
#include "../webgpu/gpu.hpp"
#include "../internal.hpp"
#include "../window.hpp"
#include "../gfx/resources.hpp"
#include "../gfx/recording.hpp"
#include "../gfx/resource_cache.hpp"
#include "../gfx/texture.hpp"
#include "gx_fmt.hpp"

#include <absl/container/flat_hash_map.h>
#include <tracy/Tracy.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cfloat>
#include <cmath>
#include <mutex>
#include <optional>
#include <utility>

static aurora::Module Log("aurora::gx");

namespace aurora::gx {
using webgpu::g_device;
using webgpu::g_graphicsConfig;

GXState g_gxState{};
wgpu::BindGroup g_emptyTextureBindGroup;

namespace {
wgpu::Sampler sEmptySampler;
wgpu::Texture sEmptyTexture;
wgpu::TextureView sEmptyTextureView;
wgpu::BindGroupLayout sTextureBindGroupLayout;
wgpu::PipelineLayout sPipelineLayout;

std::atomic<int> sPendingViewportPolicy{-1};

template <typename T>
T round_away_from_zero(float value) noexcept {
  return static_cast<T>(value < 0.0f ? std::floor(value) : std::ceil(value));
}

std::pair<f32, f32> polygon_offset_for_cull_mode(GXCullMode cullMode) noexcept {
  if (cullMode == GX_CULL_FRONT) {
    return {g_gxState.backOffset, g_gxState.backScale};
  }
  return {g_gxState.frontOffset, g_gxState.frontScale};
}

wgpu::BlendFactor to_blend_factor(GXBlendFactor fac, bool isDst) {
  switch (fac) {
    DEFAULT_FATAL("invalid blend factor {}", underlying(fac));
  case GX_BL_ZERO:
    return wgpu::BlendFactor::Zero;
  case GX_BL_ONE:
    return wgpu::BlendFactor::One;
  case GX_BL_SRCCLR: // + GX_BL_DSTCLR
    if (isDst) {
      return wgpu::BlendFactor::Src;
    } else {
      return wgpu::BlendFactor::Dst;
    }
  case GX_BL_INVSRCCLR: // + GX_BL_INVDSTCLR
    if (isDst) {
      return wgpu::BlendFactor::OneMinusSrc;
    } else {
      return wgpu::BlendFactor::OneMinusDst;
    }
  case GX_BL_SRCALPHA:
    return wgpu::BlendFactor::SrcAlpha;
  case GX_BL_INVSRCALPHA:
    return wgpu::BlendFactor::OneMinusSrcAlpha;
  case GX_BL_DSTALPHA:
    return wgpu::BlendFactor::DstAlpha;
  case GX_BL_INVDSTALPHA:
    return wgpu::BlendFactor::OneMinusDstAlpha;
  }
}

wgpu::CompareFunction to_compare_function(GXCompare func) {
  switch (func) {
    DEFAULT_FATAL("invalid depth fn {}", underlying(func));
  case GX_NEVER:
    return wgpu::CompareFunction::Never;
  case GX_LESS:
    return UseReversedZ ? wgpu::CompareFunction::Greater : wgpu::CompareFunction::Less;
  case GX_EQUAL:
    return wgpu::CompareFunction::Equal;
  case GX_LEQUAL:
    return UseReversedZ ? wgpu::CompareFunction::GreaterEqual : wgpu::CompareFunction::LessEqual;
  case GX_GREATER:
    return UseReversedZ ? wgpu::CompareFunction::Less : wgpu::CompareFunction::Greater;
  case GX_NEQUAL:
    return wgpu::CompareFunction::NotEqual;
  case GX_GEQUAL:
    return UseReversedZ ? wgpu::CompareFunction::LessEqual : wgpu::CompareFunction::GreaterEqual;
  case GX_ALWAYS:
    return wgpu::CompareFunction::Always;
  }
}

wgpu::BlendState to_blend_state(GXBlendMode mode, GXBlendFactor srcFac, GXBlendFactor dstFac, GXLogicOp op,
                                u32 dstAlpha, bool dualSource) {
  wgpu::BlendComponent colorBlendComponent;
  switch (mode) {
    DEFAULT_FATAL("unsupported blend mode {}", underlying(mode));
  case GX_BM_NONE:
    colorBlendComponent = {
        .operation = wgpu::BlendOperation::Add,
        .srcFactor = wgpu::BlendFactor::One,
        .dstFactor = wgpu::BlendFactor::Zero,
    };
    break;
  case GX_BM_BLEND:
    colorBlendComponent = {
        .operation = wgpu::BlendOperation::Add,
        .srcFactor = to_blend_factor(srcFac, false),
        .dstFactor = to_blend_factor(dstFac, true),
    };
    break;
  case GX_BM_SUBTRACT:
    colorBlendComponent = {
        .operation = wgpu::BlendOperation::ReverseSubtract,
        .srcFactor = wgpu::BlendFactor::One,
        .dstFactor = wgpu::BlendFactor::One,
    };
    break;
  case GX_BM_LOGIC:
    // WebGPU has no fixed-function integer logic op; approximate the rest with blending.
    switch (op) {
    case GX_LO_CLEAR:
      colorBlendComponent = {
          .operation = wgpu::BlendOperation::Add,
          .srcFactor = wgpu::BlendFactor::Zero,
          .dstFactor = wgpu::BlendFactor::Zero,
      };
      break;
    case GX_LO_INVAND:
      colorBlendComponent = {
          .operation = wgpu::BlendOperation::Add,
          .srcFactor = wgpu::BlendFactor::Zero,
          .dstFactor = wgpu::BlendFactor::OneMinusSrc,
      };
      break;
    case GX_LO_OR:
      colorBlendComponent = {
          .operation = wgpu::BlendOperation::Max,
          .srcFactor = wgpu::BlendFactor::One,
          .dstFactor = wgpu::BlendFactor::One,
      };
      break;
    case GX_LO_AND:
    case GX_LO_REVAND:
    case GX_LO_XOR:
    case GX_LO_NOR:
    case GX_LO_EQUIV:
    case GX_LO_INV:
    case GX_LO_REVOR:
    case GX_LO_INVCOPY:
    case GX_LO_INVOR:
    case GX_LO_NAND:
    case GX_LO_SET:
    default:
    case GX_LO_COPY:
      colorBlendComponent = {
          .operation = wgpu::BlendOperation::Add,
          .srcFactor = wgpu::BlendFactor::One,
          .dstFactor = wgpu::BlendFactor::Zero,
      };
      break;
    case GX_LO_NOOP:
      colorBlendComponent = {
          .operation = wgpu::BlendOperation::Add,
          .srcFactor = wgpu::BlendFactor::Zero,
          .dstFactor = wgpu::BlendFactor::One,
      };
      break;
    }
    break;
  }
  if (dualSource) {
    const auto remap = [](wgpu::BlendFactor factor) {
      if (factor == wgpu::BlendFactor::SrcAlpha) {
        return wgpu::BlendFactor::Src1Alpha;
      }
      if (factor == wgpu::BlendFactor::OneMinusSrcAlpha) {
        return wgpu::BlendFactor::OneMinusSrc1Alpha;
      }
      return factor;
    };
    colorBlendComponent.srcFactor = remap(colorBlendComponent.srcFactor);
    colorBlendComponent.dstFactor = remap(colorBlendComponent.dstFactor);
  }
  wgpu::BlendComponent alphaBlendComponent;
  if (dstAlpha != UINT32_MAX) {
    alphaBlendComponent = wgpu::BlendComponent{
        .operation = wgpu::BlendOperation::Add,
        .srcFactor = wgpu::BlendFactor::Constant,
        .dstFactor = wgpu::BlendFactor::Zero,
    };
  } else {
    alphaBlendComponent = colorBlendComponent;
  }
  return {
      .color = colorBlendComponent,
      .alpha = alphaBlendComponent,
  };
}

wgpu::ColorWriteMask to_write_mask(bool colorUpdate, bool alphaUpdate) {
  wgpu::ColorWriteMask writeMask = wgpu::ColorWriteMask::None;
  if (colorUpdate) {
    writeMask |= wgpu::ColorWriteMask::Red | wgpu::ColorWriteMask::Green | wgpu::ColorWriteMask::Blue;
  }
  if (alphaUpdate) {
    writeMask |= wgpu::ColorWriteMask::Alpha;
  }
  return writeMask;
}

wgpu::PrimitiveState to_primitive_state(GXCullMode gx_cullMode) {
  auto cullMode = wgpu::CullMode::None;
  switch (gx_cullMode) {
    DEFAULT_FATAL("unsupported cull mode {}", underlying(gx_cullMode));
  case GX_CULL_FRONT:
    cullMode = wgpu::CullMode::Front;
    break;
  case GX_CULL_BACK:
    cullMode = wgpu::CullMode::Back;
    break;
  case GX_CULL_NONE:
    break;
  }
  return {
      .topology = wgpu::PrimitiveTopology::TriangleList,
      .stripIndexFormat = wgpu::IndexFormat::Undefined,
      .frontFace = wgpu::FrontFace::CW,
      .cullMode = cullMode,
  };
}
} // namespace

void set_viewport_policy(AuroraViewportPolicy policy) noexcept {
  sPendingViewportPolicy.store(policy, std::memory_order_release);
}

void update() noexcept {
  if (const int pending = sPendingViewportPolicy.exchange(-1, std::memory_order_acq_rel); pending != -1) {
    const auto policy = static_cast<AuroraViewportPolicy>(pending);
    g_gxState.viewportPolicy = policy;
    window::set_frame_buffer_aspect_fit(policy == AURORA_VIEWPORT_FIT);
    window::set_force_aspect_16_9(policy == AURORA_VIEWPORT_16_9);
    window::set_present_surface_fill(policy == AURORA_VIEWPORT_STRETCH);
  }
}

Vec2<uint32_t> logical_fb_size() noexcept {
  return gfx::is_offscreen() ? gfx::get_render_target_size() : vi::configured_fb_size();
}

gfx::Viewport map_logical_viewport(const gfx::Viewport& logicalViewport) noexcept {
  if (g_gxState.viewportPolicy == AURORA_VIEWPORT_NATIVE) {
    return logicalViewport;
  }

  const auto [logicalFbWidth, logicalFbHeight] = logical_fb_size();
  const auto [targetWidth, targetHeight] = gfx::get_render_target_size();
  if (logicalFbWidth == 0 || logicalFbHeight == 0 || targetWidth == 0 || targetHeight == 0) {
    return logicalViewport;
  }

  const float scaleX = static_cast<float>(targetWidth) / static_cast<float>(logicalFbWidth);
  const float scaleY = static_cast<float>(targetHeight) / static_cast<float>(logicalFbHeight);
  return {
      .left = logicalViewport.left * scaleX,
      .top = logicalViewport.top * scaleY,
      .width = logicalViewport.width * scaleX,
      .height = logicalViewport.height * scaleY,
      .znear = logicalViewport.znear,
      .zfar = logicalViewport.zfar,
  };
}

gfx::ClipRect map_logical_scissor(const gfx::ClipRect& logicalScissor) noexcept {
  if (g_gxState.viewportPolicy == AURORA_VIEWPORT_NATIVE) {
    return logicalScissor;
  }

  const auto [logicalFbWidth, logicalFbHeight] = logical_fb_size();
  const auto [targetWidth, targetHeight] = gfx::get_render_target_size();
  if (logicalFbWidth == 0 || logicalFbHeight == 0 || targetWidth == 0 || targetHeight == 0) {
    return logicalScissor;
  }

  const float scaleX = static_cast<float>(targetWidth) / static_cast<float>(logicalFbWidth);
  const float scaleY = static_cast<float>(targetHeight) / static_cast<float>(logicalFbHeight);

  const float left = static_cast<float>(logicalScissor.x) * scaleX;
  const float top = static_cast<float>(logicalScissor.y) * scaleY;
  const float right = static_cast<float>(logicalScissor.x + logicalScissor.width) * scaleX;
  const float bottom = static_cast<float>(logicalScissor.y + logicalScissor.height) * scaleY;

  const auto mappedLeft = std::clamp(static_cast<int32_t>(std::floor(left)), 0, static_cast<int32_t>(targetWidth));
  const auto mappedTop = std::clamp(static_cast<int32_t>(std::floor(top)), 0, static_cast<int32_t>(targetHeight));
  const auto mappedRight =
      std::clamp(static_cast<int32_t>(std::ceil(right)), mappedLeft, static_cast<int32_t>(targetWidth));
  const auto mappedBottom =
      std::clamp(static_cast<int32_t>(std::ceil(bottom)), mappedTop, static_cast<int32_t>(targetHeight));

  return {
      .x = mappedLeft,
      .y = mappedTop,
      .width = mappedRight - mappedLeft,
      .height = mappedBottom - mappedTop,
  };
}

namespace {
struct ScissorRange {
  s32 offset = 0;
  s32 start = 0;
  s32 end = 0;
};

struct OffsetScissor {
  gfx::ClipRect rect{};
  s32 xOff = 0;
  s32 yOff = 0;
};

// The offset wraps every 1024 EFB pixels, so try each wrap and keep the ones that land on the EFB.
std::array<ScissorRange, 9> scissor_ranges(s32 start, s32 end, s32 offset, s32 efbDim, u32& count) noexcept {
  std::array<ScissorRange, 9> ranges{};
  count = 0;
  for (s32 extraOff = -4096; extraOff <= 4096; extraOff += 1024) {
    const s32 newOff = offset + extraOff;
    const s32 newStart = std::clamp(start - newOff, 0, efbDim);
    const s32 newEnd = std::clamp(end - newOff + 1, 0, efbDim);
    if (newStart < newEnd && count < ranges.size()) {
      ranges[count++] = {.offset = newOff, .start = newStart, .end = newEnd};
    }
  }
  return ranges;
}

int viewport_overlap(const OffsetScissor& rect, const gfx::Viewport& viewport) noexcept {
  const float vpLeft = std::min(viewport.left, viewport.left + viewport.width);
  const float vpRight = std::max(viewport.left, viewport.left + viewport.width);
  const float vpTop = std::min(viewport.top, viewport.top + viewport.height);
  const float vpBottom = std::max(viewport.top, viewport.top + viewport.height);
  const float left = std::clamp(static_cast<float>(rect.rect.x + rect.xOff), vpLeft, vpRight);
  const float right = std::clamp(static_cast<float>(rect.rect.x + rect.rect.width + rect.xOff), vpLeft, vpRight);
  const float top = std::clamp(static_cast<float>(rect.rect.y + rect.yOff), vpTop, vpBottom);
  const float bottom = std::clamp(static_cast<float>(rect.rect.y + rect.rect.height + rect.yOff), vpTop, vpBottom);
  return static_cast<int>(std::max(right - left, 0.f) * std::max(bottom - top, 0.f));
}

// Dolphin's choice among the wrapped candidates: the one covering most of the viewport.
OffsetScissor offset_scissor() noexcept {
  const auto& scissor = g_gxState.logicalScissor;
  constexpr OffsetScissor empty{.rect = {1000, 1000, 1, 1}};
  if (scissor.width <= 0 || scissor.height <= 0) {
    return empty;
  }
  const auto [fbWidth, fbHeight] = logical_fb_size();
  u32 xCount = 0;
  u32 yCount = 0;
  const auto xRanges = scissor_ranges(scissor.x, scissor.x + scissor.width - 1, g_gxState.scissorOffsetX,
                                      static_cast<s32>(fbWidth), xCount);
  const auto yRanges = scissor_ranges(scissor.y, scissor.y + scissor.height - 1, g_gxState.scissorOffsetY,
                                      static_cast<s32>(fbHeight), yCount);
  if (xCount == 0 || yCount == 0) {
    return empty;
  }
  std::optional<OffsetScissor> best;
  for (u32 x = 0; x < xCount; ++x) {
    for (u32 y = 0; y < yCount; ++y) {
      const OffsetScissor candidate{
          .rect = {xRanges[x].start, yRanges[y].start, xRanges[x].end - xRanges[x].start,
                   yRanges[y].end - yRanges[y].start},
          .xOff = xRanges[x].offset,
          .yOff = yRanges[y].offset,
      };
      if (!best) {
        best = candidate;
        continue;
      }
      const int candidateArea = viewport_overlap(candidate, g_gxState.logicalViewport);
      const int bestArea = viewport_overlap(*best, g_gxState.logicalViewport);
      if (candidateArea > bestArea ||
          (candidateArea == bestArea &&
           candidate.rect.width * candidate.rect.height > best->rect.width * best->rect.height)) {
        best = candidate;
      }
    }
  }
  return *best;
}

bool has_scissor_offset() noexcept { return g_gxState.scissorOffsetX != 0 || g_gxState.scissorOffsetY != 0; }
} // namespace

gfx::Viewport mapped_render_viewport() noexcept {
  if (!has_scissor_offset()) {
    return map_logical_viewport(g_gxState.logicalViewport);
  }
  const auto offset = offset_scissor();
  auto shifted = g_gxState.logicalViewport;
  shifted.left -= static_cast<float>(offset.xOff);
  shifted.top -= static_cast<float>(offset.yOff);
  return map_logical_viewport(shifted);
}

gfx::ClipRect mapped_render_scissor() noexcept {
  if (!has_scissor_offset()) {
    return map_logical_scissor(g_gxState.logicalScissor);
  }
  return map_logical_scissor(offset_scissor().rect);
}

void set_logical_viewport(const gfx::Viewport& viewport) noexcept {
  if (viewport.left != g_gxState.logicalViewport.left || viewport.width != g_gxState.logicalViewport.width ||
      viewport.height != g_gxState.logicalViewport.height) {
    g_gxState.dirty |= DirtyUniform;
  }
  g_gxState.logicalViewport = viewport;
  set_render_viewport(mapped_render_viewport());
  if (has_scissor_offset()) {
    set_render_scissor(mapped_render_scissor());
  }
}

void set_render_viewport(const gfx::Viewport& viewport) noexcept {
  if (viewport.left != g_gxState.renderViewport.left || viewport.width != g_gxState.renderViewport.width ||
      viewport.height != g_gxState.renderViewport.height) {
    g_gxState.dirty |= DirtyUniform;
  }
  g_gxState.renderViewport = viewport;
  gfx::set_viewport(viewport);
}

void set_logical_scissor(const gfx::ClipRect& scissor) noexcept {
  g_gxState.logicalScissor = scissor;
  set_render_scissor(mapped_render_scissor());
  if (has_scissor_offset()) {
    set_render_viewport(mapped_render_viewport());
  }
}

void set_render_scissor(const gfx::ClipRect& scissor) noexcept {
  g_gxState.renderScissor = scissor;
  gfx::set_scissor(scissor);
}

const gfx::TextureBind& get_texture(GXTexMapID id) noexcept { return g_gxState.textures[static_cast<size_t>(id)]; }

wgpu::RenderPipeline build_pipeline(const PipelineConfig& config, const gfx::RenderTargetLayout& layout,
                                    const PipelineOptions& options, ArrayRef<wgpu::VertexBufferLayout> vtxBuffers,
                                    wgpu::ShaderModule shader, const char* label) noexcept {
  ZoneScoped;
  const float depthBias = (UseReversedZ ? -1.0f : 1.0f) * std::bit_cast<float>(config.polygonOffsetBits);
  const float depthBiasSlopeScale = (UseReversedZ ? -1.0f : 1.0f) * std::bit_cast<float>(config.polygonOffsetScaleBits);
  const float depthBiasClamp = webgpu::g_hasCoreFeatures ? std::bit_cast<float>(config.polygonOffsetClampBits) : 0.0f;
  const bool writesDepth = config.depthCompare && options.depthUpdate;
  const wgpu::DepthStencilState depthStencil{
      .format = layout.depthStencilFormat,
      .depthWriteEnabled = writesDepth,
      .depthCompare = config.depthCompare ? to_compare_function(config.depthFunc) : wgpu::CompareFunction::Always,
      .depthBias = round_away_from_zero<int32_t>(depthBias),
      .depthBiasSlopeScale = depthBiasSlopeScale,
      .depthBiasClamp = depthBiasClamp,
  };
  const auto blendState = to_blend_state(config.blendMode, config.blendFacSrc, config.blendFacDst, config.blendOp,
                                         config.dstAlpha, options.dstAlphaMode == DstAlphaMode::DualSource);
  std::array<wgpu::ColorTargetState, gfx::MaxColorAttachments> colorTargets{};
  for (uint32_t i = 0; i < layout.colorAttachmentCount; ++i) {
    colorTargets[i] = {
        .format = layout.colorAttachments[i].format,
        .writeMask = layout.colorAttachments[i].semantic == gfx::ColorAttachmentSemantic::Normal && writesDepth
                         ? wgpu::ColorWriteMask::All
                         : wgpu::ColorWriteMask::None,
    };
  }
  colorTargets[gfx::SceneColorAttachmentIndex].blend = &blendState;
  colorTargets[gfx::SceneColorAttachmentIndex].writeMask = to_write_mask(options.colorUpdate, options.alphaUpdate);
  const wgpu::FragmentState fragmentState{
      .module = shader,
      .entryPoint = "fs_main",
      .targetCount = layout.colorAttachmentCount,
      .targets = colorTargets.data(),
  };
  const wgpu::RenderPipelineDescriptor descriptor{
      .label = label,
      .layout = sPipelineLayout,
      .vertex =
          {
              .module = shader,
              .entryPoint = "vs_main",
              .bufferCount = static_cast<uint32_t>(vtxBuffers.size()),
              .buffers = vtxBuffers.data(),
          },
      .primitive = to_primitive_state(config.cullMode),
      .depthStencil = layout.depthStencilFormat != wgpu::TextureFormat::Undefined ? &depthStencil : nullptr,
      .multisample = wgpu::MultisampleState{.count = layout.sampleCount},
      .fragment = &fragmentState,
  };
  return g_device.CreateRenderPipeline(&descriptor);
}

void populate_pipeline_config(PipelineConfig& config, GXPrimitive primitive, GXVtxFmt fmt) noexcept {
  ZoneScoped;

  const auto& vtxFmt = g_gxState.vtxFmts[fmt];
  config.shaderConfig = {};
	config.shaderConfig.forceAlphaOne = !g_gxState.colorUpdate && g_gxState.alphaUpdate &&
		g_gxState.dstAlpha != UINT32_MAX && g_gxState.dstAlpha != 0;
  config.shaderConfig.fogType = g_gxState.fog.type;
	config.shaderConfig.zTextureOp = g_gxState.zTextureOp;
	if (g_gxState.zTextureOp != GX_ZT_DISABLE) {
		config.shaderConfig.zTextureFormat = g_gxState.zTextureFormat;
	}
  config.shaderConfig.fogRangeEnabled = g_gxState.fog.rangeEnabled;
  u8 vtxOffset = 0;
  for (int i = GX_VA_PNMTXIDX; i <= GX_VA_TEX7; ++i) {
    const auto attr = static_cast<GXAttr>(i);
    const auto type = g_gxState.vtxDesc[i];
    auto& mapping = config.shaderConfig.attrs[i];
    if (type == GX_NONE) {
      mapping = {};
      continue;
    }
    const auto& attrFmt = vtxFmt.attrs[i];
    const auto cnt = comp_cnt_count(attr, attrFmt.cnt);
    const bool nbt3 = attr == GX_VA_NRM && attrFmt.cnt == GX_NRM_NBT3;
    mapping = AttrConfig{
        .attrType = static_cast<u8>(type),
        .cnt = cnt,
        .compType = static_cast<u8>(attrFmt.type),
        .offset = vtxOffset,
        .stride = 0,
        .frac = attrFmt.frac,
        .le = false,
        .nbt3 = nbt3,
    };
    switch (type) {
    case GX_DIRECT: {
      vtxOffset += comp_type_size(attr, attrFmt.type) * cnt;
      break;
    }
    case GX_INDEX8:
      mapping.stride = g_gxState.arrays[i].stride;
      mapping.le = g_gxState.arrays[i].le;
      vtxOffset += nbt3 ? 3 : 1;
      break;
    case GX_INDEX16:
      mapping.stride = g_gxState.arrays[i].stride;
      mapping.le = g_gxState.arrays[i].le;
      vtxOffset += nbt3 ? 6 : 2;
      break;
    default:
      Log.fatal("populate_pipeline_config: Invalid vertex type {}", type);
    }
  }
  config.shaderConfig.vtxStride = vtxOffset;
  if (primitive == GX_LINES) {
    config.shaderConfig.lineMode = 1;
  } else if (primitive == GX_LINESTRIP) {
    config.shaderConfig.lineMode = 2;
  } else if (primitive == GX_POINTS) {
    config.shaderConfig.lineMode = 3;
  } else {
    config.shaderConfig.lineMode = 0;
  }
  config.shaderConfig.tevSwapTable = g_gxState.tevSwapTable;
  for (u8 i = 0; i < g_gxState.numTevStages; ++i) {
    auto& stage = config.shaderConfig.tevStages[i];
    stage = g_gxState.tevStages[i];
    // GX reads texcoord 0 for a coordinate beyond the texgen count, and samples nothing without texgens.
    if (stage.texCoordId != GX_TEXCOORD_NULL && underlying(stage.texCoordId) >= g_gxState.numTexGens) {
      if (g_gxState.numTexGens == 0) {
        stage.texCoordId = GX_TEXCOORD_NULL;
        stage.texMapId = GX_TEXMAP_NULL;
      } else {
        stage.texCoordId = GX_TEXCOORD0;
      }
    }
    // Konstant selectors left over from earlier draws only matter when a pass reads KONST.
    const auto& c = stage.colorPass;
    if (c.a != GX_CC_KONST && c.b != GX_CC_KONST && c.c != GX_CC_KONST && c.d != GX_CC_KONST) {
      stage.kcSel = GX_TEV_KCSEL_1;
    }
    const auto& a = stage.alphaPass;
    if (a.a != GX_CA_KONST && a.b != GX_CA_KONST && a.c != GX_CA_KONST && a.d != GX_CA_KONST) {
      stage.kaSel = GX_TEV_KASEL_1;
    }
  }
  config.shaderConfig.tevStageCount = g_gxState.numTevStages;
  for (u8 i = 0; i < g_gxState.numIndStages; ++i) {
    config.shaderConfig.indStages[i] = g_gxState.indStages[i];
  }
  config.shaderConfig.numIndStages = g_gxState.numIndStages;
  for (u8 i = 0; i < MaxColorChannels; ++i) {
    const auto& cc = g_gxState.colorChannelConfig[i];
    if (cc.lightingEnabled) {
      config.shaderConfig.colorChannels[i] = cc;
    } else {
      // Only matSrc matters when lighting disabled
      config.shaderConfig.colorChannels[i] = {
          .matSrc = cc.matSrc,
      };
    }
  }
  for (u8 i = 0; i < g_gxState.numTexGens; ++i) {
    auto& tcg = config.shaderConfig.tcgs[i];
    tcg = g_gxState.tcgs[i];
    if (tcg.type < GX_TG_BUMP0 || tcg.type > GX_TG_BUMP7) {
      tcg.embossSrc = 0;
    }
    if (!g_gxState.dualTex) {
      tcg.postMtx = GX_PTIDENTITY;
      tcg.normalize = false;
    }
  }
  if (g_gxState.alphaCompare) {
    config.shaderConfig.alphaCompare = g_gxState.alphaCompare;
  }
  const auto cullMode = config.shaderConfig.lineMode == 0 ? g_gxState.cullMode : GX_CULL_NONE;
  const auto [polygonOffset, polygonOffsetScale] = polygon_offset_for_cull_mode(cullMode);
  const bool hasAlpha = efb_has_alpha(g_gxState.pixelFmt);
  const bool alphaUpdate = hasAlpha && g_gxState.alphaUpdate;
  const auto blendFactor = [hasAlpha](GXBlendFactor factor) {
    if (!hasAlpha) {
      if (factor == GX_BL_DSTALPHA) {
        return GX_BL_ONE;
      }
      if (factor == GX_BL_INVDSTALPHA) {
        return GX_BL_ZERO;
      }
    }
    return factor;
  };
  config = {
      .shaderConfig = config.shaderConfig,
      .depthFunc = g_gxState.depthFunc,
      .cullMode = cullMode,
      .blendMode = g_gxState.blendMode,
      .blendFacSrc = blendFactor(g_gxState.blendFacSrc),
      .blendFacDst = blendFactor(g_gxState.blendFacDst),
      .blendOp = g_gxState.blendOp,
      .dstAlpha = alphaUpdate ? g_gxState.dstAlpha : UINT32_MAX,
      .polygonOffsetBits = std::bit_cast<uint32_t>(polygonOffset),
      .polygonOffsetScaleBits = std::bit_cast<uint32_t>(polygonOffsetScale),
      .polygonOffsetClampBits = std::bit_cast<uint32_t>(g_gxState.clamp),
      .depthCompare = g_gxState.depthCompare,
      .depthUpdate = g_gxState.depthUpdate,
      .alphaUpdate = alphaUpdate,
      .colorUpdate = g_gxState.colorUpdate,
  };
}

GXBindGroups build_bind_groups(const ShaderInfo& info) noexcept {
  ZoneScoped;

  if (!info.sampledTextures.any() && !info.sampledIndTextures.any()) {
    // Don't bother re-binding anything
    return {};
  }

  // Using C WGPU types instead of C++ wrappers to avoid destructor overhead
  std::array<WGPUBindGroupEntry, MaxTextures * 2> textureEntries{};
  for (u32 i = 0; i < MaxTextures; ++i) {
    const auto& tex = g_gxState.textures[i];
    WGPUBindGroupEntry& textureEntry = textureEntries[i * 2];
    WGPUBindGroupEntry& samplerEntry = textureEntries[i * 2 + 1];
    textureEntry.binding = i * 2;
    samplerEntry.binding = i * 2 + 1;
    if (tex && (info.sampledTextures[i] || info.sampledIndTextures[i])) {
      textureEntry.textureView = tex.ref->sampleTextureView.Get();
      samplerEntry.sampler = gfx::sampler_ref(tex.get_descriptor()).Get();
    } else {
      textureEntry.textureView = sEmptyTextureView.Get();
      samplerEntry.sampler = sEmptySampler.Get();
    }
  }
  const WGPUBindGroupDescriptor textureBindGroupDescriptor{
      .label = {"GX Texture Bind Group", WGPU_STRLEN},
      .layout = sTextureBindGroupLayout.Get(),
      .entryCount = textureEntries.size(),
      .entries = textureEntries.data(),
  };
  return {
      .textureBindGroup = gfx::bind_group_ref(textureBindGroupDescriptor),
  };
}

void initialize() noexcept {
  {
    std::array<wgpu::BindGroupLayoutEntry, MaxTextures * 2> textureEntries;
    for (u32 i = 0; i < MaxTextures; ++i) {
      textureEntries[i * 2] = {
          .binding = i * 2,
          .visibility = wgpu::ShaderStage::Fragment,
          .texture =
              {
                  .sampleType = wgpu::TextureSampleType::Float,
                  .viewDimension = wgpu::TextureViewDimension::e2D,
              },
      };
      textureEntries[i * 2 + 1] = {
          .binding = i * 2 + 1,
          .visibility = wgpu::ShaderStage::Fragment,
          .sampler = {.type = wgpu::SamplerBindingType::Filtering},
      };
    }
    const wgpu::BindGroupLayoutDescriptor descriptor{
        .label = "GX Texture Bind Group Layout",
        .entryCount = textureEntries.size(),
        .entries = textureEntries.data(),
    };
    sTextureBindGroupLayout = g_device.CreateBindGroupLayout(&descriptor);
  }
  {
    constexpr wgpu::SamplerDescriptor descriptor{.label = "Empty sampler"};
    sEmptySampler = gfx::sampler_ref(descriptor);
  }
  {
    constexpr wgpu::TextureDescriptor descriptor{
        .label = "Empty texture",
        .usage = wgpu::TextureUsage::TextureBinding,
        .size = {1, 1},
        .format = wgpu::TextureFormat::RGBA8Unorm,
    };
    sEmptyTexture = g_device.CreateTexture(&descriptor);
    sEmptyTextureView = sEmptyTexture.CreateView();
  }
  {
    std::array<wgpu::BindGroupEntry, MaxTextures * 2> entries;
    for (u32 i = 0; i < MaxTextures; ++i) {
      entries[i * 2] = {
          .binding = i * 2,
          .textureView = sEmptyTextureView,
      };
      entries[i * 2 + 1] = {
          .binding = i * 2 + 1,
          .sampler = sEmptySampler,
      };
    }
    const wgpu::BindGroupDescriptor desc{
        .label = "GX Empty Texture Bind Group",
        .layout = sTextureBindGroupLayout,
        .entryCount = entries.size(),
        .entries = entries.data(),
    };
    g_emptyTextureBindGroup = g_device.CreateBindGroup(&desc);
  }
  {
    const std::array layouts{
        gfx::detail::resources().staticBindGroupLayout,
        gfx::detail::resources().uniformBindGroupLayout,
        sTextureBindGroupLayout,
    };
    const wgpu::PipelineLayoutDescriptor desc{
        .label = "GX Pipeline Layout",
        .bindGroupLayoutCount = layouts.size(),
        .bindGroupLayouts = layouts.data(),
        .immediateSize = sizeof(DrawImmediateData),
    };
    sPipelineLayout = g_device.CreatePipelineLayout(&desc);
  }
}

void shutdown() noexcept {
  // TODO we should probably store this all in g_state.gx instead
  sTextureBindGroupLayout = {};
  for (auto& item : g_gxState.textures) {
    item.ref.reset();
  }
  g_gxState.loadedTextures.fill({});
  g_gxState.loadedTluts.fill({});
  texture::shutdown();
}
} // namespace aurora::gx
