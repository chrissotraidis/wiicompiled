#include "kartpad_selfcheck.hpp"

#include "common.hpp"
#include "../internal.hpp"
#include "../webgpu/gpu.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <mutex>

#if defined(__ANDROID__)
#include <sys/system_properties.h>
#endif

namespace aurora::gfx::kartpad_selfcheck {
namespace {
Module Log("aurora::gfx::selfcheck");

using Clock = std::chrono::steady_clock;
constexpr auto SettleTime = std::chrono::seconds(20); // past boot, menus and first shader compiles
constexpr auto RetryDelay = std::chrono::seconds(10);
constexpr auto StaleTime = std::chrono::seconds(10);  // a recorded check whose frame never rendered
constexpr unsigned MaxAttempts = 10;
// Fewer covered pixels than this cannot tell a broken draw from a working one (a missing or
// white model still differs in every covered pixel, well past the 16-pixel mismatch floor).
constexpr uint64_t MinDrawnPixels = 32;

enum class State { Waiting, Recorded, Captured, Encoded, Mapping, Done };

struct Shared {
  std::mutex mutex;
  State state = State::Waiting;
  Clock::time_point readyAt{};
  Clock::time_point stateSince{};
  unsigned attempts = 0;
  gx::DrawData draws[2]{}; // [0] as the game drew it, [1] the other vertex layout
  bool mainRepacked = false;
  uint32_t vertices = 0;
  Viewport viewport{};
  bool hasViewport = false;
  ClipRect scissor{};
  bool hasScissor = false;
  wgpu::Extent3D size{};
  uint32_t samples = 1;
  uint32_t bytesPerRow = 0;
  wgpu::Buffer readback[2];
  int mapped = 0;
  bool mapFailed = false;
};

Shared& shared() {
  static Shared s;
  return s;
}

// Set once the check is finished, so the per-draw question costs one load afterwards.
std::atomic<bool> g_finished{false};

bool env_is(const char* name, const char* value) {
  const char* v = std::getenv(name);
  return v != nullptr && std::strcmp(v, value) == 0;
}

void set_state(Shared& s, State state) {
  s.state = state;
  s.stateSince = Clock::now();
  if (state == State::Done) g_finished.store(true, std::memory_order_relaxed);
}

// Back to waiting for another draw, unless the attempts are used up. One short line per attempt,
// so a log shows where the check stopped.
void retry(Shared& s, const char* reason) {
  Log.info("KartPad draw self-check: attempt={} retry reason={}{}", s.attempts, reason,
           s.attempts >= MaxAttempts ? " result=inconclusive" : "");
  set_state(s, s.attempts >= MaxAttempts ? State::Done : State::Waiting);
  s.readyAt = Clock::now() + RetryDelay;
}

void compare(Shared& s) {
  const uint32_t w = s.size.width;
  const uint32_t h = s.size.height;
  const auto* a = static_cast<const uint8_t*>(s.readback[0].GetConstMappedRange(0, uint64_t(s.bytesPerRow) * h));
  const auto* b = static_cast<const uint8_t*>(s.readback[1].GetConstMappedRange(0, uint64_t(s.bytesPerRow) * h));
  uint64_t drawnA = 0, drawnB = 0, differing = 0, strong = 0;
  if (a != nullptr && b != nullptr) {
    for (uint32_t y = 0; y < h; ++y) {
      const uint8_t* ra = a + uint64_t(y) * s.bytesPerRow;
      const uint8_t* rb = b + uint64_t(y) * s.bytesPerRow;
      for (uint32_t x = 0; x < w; ++x) {
        const uint8_t* pa = ra + x * 4;
        const uint8_t* pb = rb + x * 4;
        const bool hasA = (pa[0] | pa[1] | pa[2] | pa[3]) != 0;
        const bool hasB = (pb[0] | pb[1] | pb[2] | pb[3]) != 0;
        drawnA += hasA;
        drawnB += hasB;
        int maxDiff = 0;
        for (int c = 0; c < 4; ++c) maxDiff = std::max(maxDiff, std::abs(int(pa[c]) - int(pb[c])));
        differing += maxDiff != 0;
        strong += maxDiff > 16;
      }
    }
  }
  s.readback[0].Unmap();
  s.readback[1].Unmap();
  const uint64_t drawn = std::max(drawnA, drawnB);
  const bool tooSmall = drawn < MinDrawnPixels;
  const char* result = tooSmall ? "inconclusive" : (strong > std::max<uint64_t>(16, drawn / 100) ? "mismatch" : "match");
  Log.info("KartPad draw self-check: adapter_qualcomm={} adreno={} game_layout={} vertices={} target={}x{}x{} "
           "drawn_game={} drawn_other={} differing={} strongly_differing={} attempt={} result={}",
           webgpu::g_adapterIsQualcomm, webgpu::g_adapterAdrenoModel, s.mainRepacked ? "cpu_repack" : "shader_fetch",
           s.vertices, w, h, s.samples, drawnA, drawnB, differing, strong, s.attempts, result);
  s.readback[0] = {};
  s.readback[1] = {};
  if (tooSmall) {
    retry(s, drawn == 0 ? "nothing_drawn" : "too_small"); // off screen, culled or tiny; try another draw
  } else {
    set_state(s, State::Done);
  }
}
} // namespace

bool wanted() noexcept {
  static const bool disabled = env_is("KARTPAD_DRAW_SELFCHECK", "0");
  if (disabled || g_finished.load(std::memory_order_relaxed)) return false;
  auto& s = shared();
  std::lock_guard lock(s.mutex);
  const auto now = Clock::now();
  if ((s.state == State::Recorded || s.state == State::Captured) && now - s.stateSince > StaleTime) {
    retry(s, s.state == State::Recorded ? "not_rendered" : "not_encoded");
  }
  if (s.state != State::Waiting || s.attempts >= MaxAttempts) return false;
  if (s.readyAt == Clock::time_point{}) {
    s.readyAt = now + SettleTime;
    Log.info("KartPad draw self-check: armed, first character draw seen");
    return false;
  }
  return now >= s.readyAt;
}

void skipped(const char* reason) noexcept {
  static bool logged = false;
  if (logged) return;
  logged = true;
  Log.info("KartPad draw self-check: skipped a draw reason={}", reason);
}

bool break_twin() noexcept {
  static const bool value = [] {
#if defined(__ANDROID__)
    // Test hook only: `adb shell setprop debug.kartpad.selfcheck_break 1` before launch.
    char prop[PROP_VALUE_MAX] = {};
    if (__system_property_get("debug.kartpad.selfcheck_break", prop) > 0 && std::strcmp(prop, "1") == 0) return true;
#endif
    return env_is("KARTPAD_DRAW_SELFCHECK_BREAK", "1");
  }();
  return value;
}

void record(const gx::DrawData& main, const gx::DrawData& twin, bool mainRepacked, uint32_t vertices) {
  auto& s = shared();
  {
    std::lock_guard lock(s.mutex);
    if (s.state != State::Waiting) return;
    s.draws[0] = main;
    s.draws[1] = twin;
    s.mainRepacked = mainRepacked;
    s.vertices = vertices;
    ++s.attempts;
    set_state(s, State::Recorded);
  }
  push_selfcheck_marker();
}

void capture(const Viewport* viewport, const ClipRect* scissor, wgpu::Extent3D targetSize, uint32_t samples) {
  auto& s = shared();
  std::lock_guard lock(s.mutex);
  if (s.state != State::Recorded) return;
  s.hasViewport = viewport != nullptr;
  if (viewport != nullptr) s.viewport = *viewport;
  s.hasScissor = scissor != nullptr;
  if (scissor != nullptr) s.scissor = *scissor;
  s.size = targetSize;
  s.samples = samples;
  set_state(s, State::Captured);
}

void encode(const wgpu::CommandEncoder& cmd, const wgpu::BindGroup& staticBindGroup) {
  auto& s = shared();
  std::lock_guard lock(s.mutex);
  if (s.state != State::Captured) return;
  const uint32_t w = s.size.width;
  const uint32_t h = s.size.height;
  if (w == 0 || h == 0) {
    retry(s, "empty_target");
    return;
  }
  s.bytesPerRow = (w * 4 + 255) & ~255u;
  const auto colorFormat = webgpu::g_graphicsConfig.surfaceConfiguration.format;
  bool pipelinesReady = true;
  const auto make = [&](uint32_t samples, wgpu::TextureFormat format, wgpu::TextureUsage usage) {
    const wgpu::TextureDescriptor desc{
        .label = "KartPad self-check target",
        .usage = usage,
        .dimension = wgpu::TextureDimension::e2D,
        .size = {w, h, 1},
        .format = format,
        .mipLevelCount = 1,
        .sampleCount = samples,
    };
    return webgpu::g_device.CreateTexture(&desc);
  };
  for (int k = 0; k < 2; ++k) {
    const bool msaa = s.samples > 1;
    const auto color = make(s.samples, colorFormat,
                            wgpu::TextureUsage::RenderAttachment | (msaa ? wgpu::TextureUsage::None : wgpu::TextureUsage::CopySrc));
    const auto resolved = msaa ? make(1, colorFormat, wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::CopySrc) : color;
    const auto depth = make(s.samples, webgpu::g_graphicsConfig.depthFormat, wgpu::TextureUsage::RenderAttachment);
    const auto colorView = color.CreateView();
    const auto resolveView = msaa ? resolved.CreateView() : wgpu::TextureView{};
    const auto depthView = depth.CreateView();
    const std::array attachments{wgpu::RenderPassColorAttachment{
        .view = colorView,
        .resolveTarget = resolveView,
        .loadOp = wgpu::LoadOp::Clear,
        .storeOp = wgpu::StoreOp::Store,
        .clearValue = {0.0, 0.0, 0.0, 0.0},
    }};
    const wgpu::RenderPassDepthStencilAttachment depthAttachment{
        .view = depthView,
        .depthLoadOp = wgpu::LoadOp::Clear,
        .depthStoreOp = wgpu::StoreOp::Discard,
        .depthClearValue = gx::UseReversedZ ? 0.f : 1.f,
    };
    const wgpu::RenderPassDescriptor passDesc{
        .label = "KartPad self-check pass",
        .colorAttachmentCount = attachments.size(),
        .colorAttachments = attachments.data(),
        .depthStencilAttachment = &depthAttachment,
    };
    auto pass = cmd.BeginRenderPass(&passDesc);
    pass.SetBindGroup(0, staticBindGroup);
    pass.SetBindGroup(2, gx::g_emptyTextureBindGroup);
    if (s.hasViewport) {
      const auto& vp = s.viewport;
      // Same reversed-Z mapping as the main pass's SetViewport.
      float minDepth = std::clamp(gx::UseReversedZ ? 1.0f - vp.zfar : vp.znear, 0.0f, 1.0f);
      float maxDepth = std::clamp(gx::UseReversedZ ? 1.0f - vp.znear : vp.zfar, 0.0f, 1.0f);
      if (minDepth > maxDepth) minDepth = maxDepth;
      pass.SetViewport(vp.left, vp.top, vp.width, vp.height, minDepth, maxDepth);
    }
    if (s.hasScissor) {
      const auto& sc = s.scissor;
      const auto left = std::clamp(sc.x, 0, static_cast<int32_t>(w));
      const auto top = std::clamp(sc.y, 0, static_cast<int32_t>(h));
      const auto right = std::clamp(sc.x + sc.width, left, static_cast<int32_t>(w));
      const auto bottom = std::clamp(sc.y + sc.height, top, static_cast<int32_t>(h));
      pass.SetScissorRect(static_cast<uint32_t>(left), static_cast<uint32_t>(top),
                          static_cast<uint32_t>(right - left), static_cast<uint32_t>(bottom - top));
    }
    gx::DrawEncodeState state{};
    state.boundTextureBindGroup = gx::g_emptyTextureBindGroup.Get();
    // A pipeline that is not ready would leave one image empty and read as a mismatch, so the
    // binding is checked here and the attempt retried instead. Not waiting for it keeps the
    // check from stalling a frame while the other layout's shader compiles.
    if (bind_pipeline(s.draws[k].pipeline, pass, state.currentPipeline, false)) {
      gx::render(s.draws[k], pass, state, false);
    } else {
      pipelinesReady = false;
    }
    pass.End();

    const wgpu::BufferDescriptor bufferDesc{
        .label = "KartPad self-check readback",
        .usage = wgpu::BufferUsage::CopyDst | wgpu::BufferUsage::MapRead,
        .size = uint64_t(s.bytesPerRow) * h,
    };
    s.readback[k] = webgpu::g_device.CreateBuffer(&bufferDesc);
    const wgpu::TexelCopyTextureInfo source{.texture = resolved};
    const wgpu::TexelCopyBufferInfo destination{
        .layout = {.offset = 0, .bytesPerRow = s.bytesPerRow, .rowsPerImage = h},
        .buffer = s.readback[k],
    };
    const wgpu::Extent3D extent{w, h, 1};
    cmd.CopyTextureToBuffer(&source, &destination, &extent);
  }
  if (pipelinesReady) {
    set_state(s, State::Encoded);
  } else {
    s.readback[0] = {};
    s.readback[1] = {};
    retry(s, "pipeline_not_ready");
  }
}

void after_submit() noexcept {
  auto& s = shared();
  wgpu::Buffer buffers[2];
  uint64_t size = 0;
  {
    std::lock_guard lock(s.mutex);
    if (s.state != State::Encoded) return;
    set_state(s, State::Mapping);
    s.mapped = 0;
    s.mapFailed = false;
    size = uint64_t(s.bytesPerRow) * s.size.height;
    buffers[0] = s.readback[0];
    buffers[1] = s.readback[1];
  }
  // Outside the lock: a spontaneous callback may run inside MapAsync and takes the lock itself.
  for (int k = 0; k < 2; ++k) {
    buffers[k].MapAsync(wgpu::MapMode::Read, 0, size, wgpu::CallbackMode::AllowSpontaneous,
                           [](wgpu::MapAsyncStatus status, wgpu::StringView) {
                             auto& sh = shared();
                             std::lock_guard inner(sh.mutex);
                             if (sh.state != State::Mapping) return;
                             if (status != wgpu::MapAsyncStatus::Success) sh.mapFailed = true;
                             if (++sh.mapped < 2) return;
                             if (sh.mapFailed) {
                               Log.warn("KartPad draw self-check: readback failed");
                               sh.readback[0] = {};
                               sh.readback[1] = {};
                               set_state(sh, State::Done);
                               return;
                             }
                             compare(sh);
                           });
  }
}

} // namespace aurora::gfx::kartpad_selfcheck
