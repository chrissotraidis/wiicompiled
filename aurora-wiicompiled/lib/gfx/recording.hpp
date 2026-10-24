#pragma once

#include "types.hpp"

#include <cstddef>
#include <span>
#include <utility>
#include <memory>
#include <string>

namespace aurora::gfx::detail {

struct FramePacket;

struct RecordedFrame {
  FramePacket* packet;
  size_t frameSlot;
};

void begin_recording(FramePacket& packet, size_t frameSlot);
RecordedFrame end_recording();
void shutdown_recording();
void increment_merged_draw_count() noexcept;

namespace testing {
struct OffscreenCacheStats {
  uint64_t bytes;
  size_t entries;
};
void suppress_render_worker(bool suppress) noexcept;
void seed_offscreen_cache(uint32_t width, uint32_t height, wgpu::TextureFormat colorFormat,
                          wgpu::TextureFormat depthFormat);
OffscreenCacheStats offscreen_cache_stats() noexcept;
bool is_offscreen_cached(uint32_t width, uint32_t height, wgpu::TextureFormat colorFormat,
                         wgpu::TextureFormat depthFormat);
} // namespace testing

} // namespace aurora::gfx::detail

namespace aurora::gfx::readback {
struct Job;
}

namespace aurora::gfx {
struct ColorPassDescriptor {
  const char* label = nullptr;
  wgpu::TextureView colorView;
  wgpu::TextureView resolveView;
  wgpu::TextureFormat colorFormat = wgpu::TextureFormat::Undefined;
  wgpu::TextureView depthStencilView;
  wgpu::TextureFormat depthStencilFormat = wgpu::TextureFormat::Undefined;
  wgpu::Extent3D targetSize;
  uint32_t sampleCount = 1;
  wgpu::LoadOp colorLoadOp = wgpu::LoadOp::Clear;
  wgpu::StoreOp colorStoreOp = wgpu::StoreOp::Store;
  wgpu::Color clearColor{0.f, 0.f, 0.f, 0.f};
  bool hasDepth = false;
  wgpu::LoadOp depthLoadOp = wgpu::LoadOp::Undefined;
  wgpu::StoreOp depthStoreOp = wgpu::StoreOp::Undefined;
  float depthClearValue = 0.f;
  bool hasStencil = false;
  wgpu::LoadOp stencilLoadOp = wgpu::LoadOp::Undefined;
  wgpu::StoreOp stencilStoreOp = wgpu::StoreOp::Undefined;
  uint32_t stencilClearValue = 0;
};

bool push_encoder_task_from_fifo(EncoderTaskId type, const void* payload, size_t payloadSize);
bool queue_draw_sync(uint16_t token, void (*callback)(uint16_t));

void finish();
void begin_color_pass(const ColorPassDescriptor& desc);
void end_color_pass();
void queue_texture_copy(wgpu::TexelCopyTextureInfo src, wgpu::TexelCopyTextureInfo dst, wgpu::Extent3D size);
void begin_offscreen(uint32_t width, uint32_t height);
void end_offscreen();
bool has_normal_attachment() noexcept;
RenderTargetLayout get_render_target_layout() noexcept;
namespace tex_palette_conv {
struct ConvRequest;
}
void queue_palette_conv(tex_palette_conv::ConvRequest req);

Range push_verts(const uint8_t* data, size_t length, size_t alignment);
template <typename T>
Range push_verts(ArrayRef<T> data, size_t alignment) {
  return push_verts(reinterpret_cast<const uint8_t*>(data.data()), data.size() * sizeof(T), alignment);
}
Range push_indices(const uint8_t* data, size_t length, size_t alignment);
template <typename T>
Range push_indices(ArrayRef<T> data, size_t alignment) {
  return push_indices(reinterpret_cast<const uint8_t*>(data.data()), data.size() * sizeof(T), alignment);
}
Range push_uniform(const uint8_t* data, size_t length);
// Reserves uniform space to be written later in this frame.
std::pair<std::span<uint8_t>, Range> map_uniform(size_t length);
// Whether `count` more aligned uniform blocks of `length` bytes fit while leaving `reserve` bytes free.
bool uniform_space_available(size_t length, size_t count, size_t reserve) noexcept;
// Staged bytes of a range pushed this frame.
const uint8_t* uniform_data(const Range& range) noexcept;
const uint8_t* vertex_data(const Range& range) noexcept;
template <typename T>
Range push_uniform(const T& data) {
  return push_uniform(reinterpret_cast<const uint8_t*>(&data), sizeof(T));
}
Range push_storage(const uint8_t* data, size_t length);
template <typename T>
Range push_storage(ArrayRef<T> data) {
  return push_storage(reinterpret_cast<const uint8_t*>(data.data()), data.size() * sizeof(T));
}
template <typename T>
Range push_storage(const T& data) {
  return push_storage(reinterpret_cast<const uint8_t*>(&data), sizeof(T));
}
Range push_texture_data(const uint8_t* data, uint32_t bytesPerRow, uint32_t rowsPerImage);

template <typename DrawData>
void push_draw_command(DrawData data);
template <typename DrawData>
DrawData* get_last_draw_command();
template <typename PipelineConfig>
PipelineRef pipeline_ref(const PipelineConfig& config);

// GX copy pipeline settings applied while resolving.
struct ResolveOptions {
  // Vertical copy filter taps (prev, current, next); {0, 64, 0} is the identity.
  std::array<u32, 3> copyFilter{0, 64, 0};
  // Distance between filter taps in EFB rows of the scaled render target.
  float copyFilterRowStride = 1.f;
  bool clampTop = false;
  bool clampBottom = false;
  bool persistent = false;
};

void resolve_pass_into(TextureHandle texture, ClipRect rect, bool clearColor, bool clearAlpha, bool clearDepth,
                       Vec4<float> clearColorValue, float clearDepthValue, GXTexFmt resolveFormat,
                       GXPixelFmt sourceFormat, const ResolveOptions& options = {});
uint32_t align_uniform(uint32_t value);
Vec2<uint32_t> get_render_target_size() noexcept;
void set_viewport(const Viewport& viewport) noexcept;
void set_scissor(const ClipRect& scissor) noexcept;
void push_debug_group(std::string label);
void insert_debug_marker(std::string label);

struct ReadbackCapture {
  bool active = false;
  bool offscreen = false;
  uint32_t width = 0;
  uint32_t height = 0;
};
ReadbackCapture readback_capture() noexcept;
bool push_readback_op(std::shared_ptr<readback::Job> job, bool efbSource);
} // namespace aurora::gfx
