#include "pipeline.hpp"
#include "kartpad_draw_inputs.hpp"
#include <aurora/kartpad_diagnostics.h>
#include <cstdlib>
#include <cstring>

#include "../webgpu/gpu.hpp"
#include "gx_fmt.hpp"
#include "shader_info.hpp"
#include "tracy/Tracy.hpp"

#include <condition_variable>
#include <memory>
#include <mutex>

#include <absl/container/flat_hash_map.h>

namespace aurora::gx {
static Module Log("aurora::gx");

namespace {
struct ShaderConfigHash {
  size_t operator()(const ShaderConfig& config) const noexcept { return static_cast<size_t>(xxh3_hash(config)); }
};

struct CachedShaderModule {
  std::condition_variable ready;
  wgpu::ShaderModule module;
  bool compiling = true;
};

std::mutex sShaderModuleCacheMutex;
absl::flat_hash_map<ShaderConfig, std::shared_ptr<CachedShaderModule>, ShaderConfigHash> sShaderModuleCache;

wgpu::ShaderModule cached_shader_module(const ShaderConfig& config) {
  std::shared_ptr<CachedShaderModule> entry;
  {
    std::unique_lock lock{sShaderModuleCacheMutex};
    const auto it = sShaderModuleCache.find(config);
    if (it == sShaderModuleCache.end()) {
      entry = std::make_shared<CachedShaderModule>();
      sShaderModuleCache.emplace(config, entry);
    } else {
      entry = it->second;
      entry->ready.wait(lock, [&] { return !entry->compiling; });
      return entry->module;
    }
  }

  auto module = build_shader(config);
  {
    std::lock_guard lock{sShaderModuleCacheMutex};
    entry->module = module;
    entry->compiling = false;
  }
  entry->ready.notify_all();
  return module;
}
} // namespace

wgpu::RenderPipeline create_pipeline(const PipelineConfig& config) {
  ZoneScoped;
  const auto shader = cached_shader_module(config.shaderConfig);
  return build_pipeline(config, {}, shader, "GX Pipeline");
}

void clear_shader_module_cache() {
  std::lock_guard lock{sShaderModuleCacheMutex};
  sShaderModuleCache.clear();
}

// Producer-side input/binding records cannot establish that render() issued a
// draw. Keep the two report-grounded recipes correlated through frame sealing.
static void diagnostic_draw_outcome(const DrawData& data, bool encoded, bool interpolated) {
  static const bool enabled = [] {
    const char* v = std::getenv("KARTPAD_RENDERER_VALIDATION");
    return v && std::strcmp(v, "1") == 0;
  }();
  if (!enabled || data.diagnosticOriginalPipeline == 0) return;
  static thread_local kartpad::diagnostics::DrawOutcomeWindow windows[2];
  auto& window = windows[data.diagnosticOriginalPipeline == 0x58866e32bada1f83ULL ? 0 : 1];
  const auto now = kartpad::diagnostics::monotonic_ms();
  if (!window.record(encoded, now)) return;
  Log.info("KartPadDrawOutcome pid={} unix_ms={} steady_ms={} original={:016x} pipeline={:016x} encoded_calls={} skipped_pipeline_calls={} last_encoded={} last_interpolated={} last_indices={} last_instances={} report={} final={}",
           kartpad::diagnostics::pid(), kartpad::diagnostics::unix_ms(), now,
           data.diagnosticOriginalPipeline, data.pipeline, window.encoded, window.skipped,
           encoded, interpolated, data.indexCount, data.instanceCount, window.reports, window.reports == 120);
  window.clearCounts();
}

void render(const DrawData& data, const wgpu::RenderPassEncoder& pass, DrawEncodeState& state,
            bool requireReadyPipeline, const gfx::Range* uniformRangeOverride) {
  if (!gfx::bind_pipeline(data.pipeline, pass, state.currentPipeline, requireReadyPipeline)) {
    diagnostic_draw_outcome(data, false, uniformRangeOverride != nullptr);
    return;
  }

  // An interpolated presentation slot re-encodes the identical draw with only this range replaced; overriding here avoids copying the whole DrawData per draw per slot.
  const gfx::Range& uniformRange = uniformRangeOverride != nullptr ? *uniformRangeOverride : data.uniformRange;
  const std::array offsets{uniformRange.offset};
  pass.SetBindGroup(1, gfx::g_uniformBindGroup, offsets.size(), offsets.data());
  // Resolved when the draw was recorded; see GXBindGroups.
  if (data.bindGroups.resolvedTextureBindGroup != nullptr &&
      data.bindGroups.resolvedTextureBindGroup != state.boundTextureBindGroup) {
    wgpuRenderPassEncoderSetBindGroup(pass.Get(), 2, data.bindGroups.resolvedTextureBindGroup, 0, nullptr);
    state.boundTextureBindGroup = data.bindGroups.resolvedTextureBindGroup;
  }
  if (data.dstAlpha != UINT32_MAX) {
    const wgpu::Color color{0.f, 0.f, 0.f, data.dstAlpha / 255.f};
    pass.SetBlendConstant(&color);
  }
  if (!state.indexBufferBound) {
    // Bound once for the pass; draws select their range with firstIndex below.
    pass.SetIndexBuffer(gfx::g_indexBuffer, wgpu::IndexFormat::Uint16, 0, wgpu::kWholeSize);
    state.indexBufferBound = true;
  }
  pass.DrawIndexed(data.indexCount, data.instanceCount,
                   static_cast<uint32_t>(data.idxRange.offset / sizeof(uint16_t)));
  diagnostic_draw_outcome(data, true, uniformRangeOverride != nullptr);
}
} // namespace aurora::gx
