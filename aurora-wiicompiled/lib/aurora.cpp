#include <aurora/aurora.h>
#include <aurora/time.hpp>
#include "audio.hpp"
#include "video.hpp"

#ifdef AURORA_ENABLE_GX
#include "gfx/resources.hpp"
#include "gfx/presentation.hpp"
#include <dolphin/gx.h>
#include <aurora/texture.hpp>
#include "gfx/frame.hpp"
#include "gfx/depth_peek.hpp"
#include "gfx/recording.hpp"
#include "gfx/render_worker.hpp"
#include "gx/command_processor.hpp"
#include "gx/fifo.hpp"
#include "gx/gx.hpp"
#include "gx/texture.hpp"
#include "imgui.hpp"
#include "webgpu/gpu.hpp"
#include "webgpu/gpu_prof.hpp"
#include <webgpu/webgpu_cpp.h>
#endif

#ifdef AURORA_ENABLE_RMLUI
#include "rmlui.hpp"
#endif

#include "gamepad.hpp"
#include "internal.hpp"
#include "io.hpp"
#include "thread.hpp"
#include "window.hpp"

#include <SDL3/SDL_filesystem.h>
#include <magic_enum.hpp>

#include "system_info.hpp"
#include "tracy/Tracy.hpp"

namespace aurora {
AuroraConfig g_config;
uint32_t g_sdlCustomEventsStart;
char g_gameName[4];

namespace {
constexpr Module Log{"aurora"};

#ifdef AURORA_ENABLE_GX
// GPU
using webgpu::g_device;
using webgpu::g_queue;
using webgpu::g_surface;

uint32_t clamp_scissor_coord(double value, uint32_t maximum) noexcept {
  if (!std::isfinite(value)) {
    return 0;
  }

  return static_cast<uint32_t>(std::clamp(value, 0.0, static_cast<double>(maximum)));
}

void set_present_viewport(const wgpu::RenderPassEncoder& pass, const gfx::Viewport& viewport, uint32_t surfaceWidth,
                          uint32_t surfaceHeight) noexcept {
  pass.SetViewport(viewport.left, viewport.top, viewport.width, viewport.height, viewport.znear, viewport.zfar);
  const auto scissorX = clamp_scissor_coord(std::floor(viewport.left), surfaceWidth);
  const auto scissorY = clamp_scissor_coord(std::floor(viewport.top), surfaceHeight);
  const auto scissorRight = clamp_scissor_coord(std::ceil(viewport.left + viewport.width), surfaceWidth);
  const auto scissorBottom = clamp_scissor_coord(std::ceil(viewport.top + viewport.height), surfaceHeight);
  pass.SetScissorRect(scissorX, scissorY, scissorRight - scissorX, scissorBottom - scissorY);
}
#endif

#ifdef AURORA_ENABLE_GX
constexpr std::array PreferredBackendOrder{
#ifdef ENABLE_BACKEND_WEBGPU
    BACKEND_WEBGPU,
#endif
#ifdef DAWN_ENABLE_BACKEND_D3D12
    BACKEND_D3D12,
#endif
#ifdef DAWN_ENABLE_BACKEND_METAL
    BACKEND_METAL,
#endif
#ifdef DAWN_ENABLE_BACKEND_VULKAN
    BACKEND_VULKAN,
#endif
#ifdef DAWN_ENABLE_BACKEND_D3D11
    BACKEND_D3D11,
#endif
// #ifdef DAWN_ENABLE_BACKEND_DESKTOP_GL
//     BACKEND_OPENGL,
// #endif
#ifdef DAWN_ENABLE_BACKEND_OPENGLES
    BACKEND_OPENGLES,
#endif
#ifdef DAWN_ENABLE_BACKEND_NULL
    BACKEND_NULL,
#endif
};
#else
constexpr std::array<AuroraBackend, 0> PreferredBackendOrder{};
#endif

bool g_initialFrame = false;

constexpr const char* backend_name(AuroraBackend backend) noexcept {
  switch (backend) {
  case BACKEND_AUTO:
    return "Auto";
  case BACKEND_D3D11:
    return "D3D11";
  case BACKEND_D3D12:
    return "D3D12";
  case BACKEND_METAL:
    return "Metal";
  case BACKEND_VULKAN:
    return "Vulkan";
  case BACKEND_OPENGL:
    return "OpenGL";
  case BACKEND_OPENGLES:
    return "OpenGLES";
  case BACKEND_WEBGPU:
    return "WebGPU";
  case BACKEND_NULL:
    return "Null";
  }
  return "Unknown";
}
#ifdef AURORA_ENABLE_GX
texture::ReplacementGroup g_textureReplacements;
#endif

AuroraInfo initialize(int argc, char* argv[], const AuroraConfig& config) noexcept {
  g_config = config;
  Log.info("Aurora initializing");
  log_system_information();

  if (g_config.appName == nullptr) {
    g_config.appName = "Aurora";
  } else {
    g_config.appName = strdup(g_config.appName);
  }

  if (g_config.userPath == nullptr) {
    g_config.userPath = SDL_GetPrefPath(nullptr, g_config.appName);
  } else {
    g_config.userPath = strdup(g_config.userPath);
  }

  if (g_config.cachePath == nullptr) {
    g_config.cachePath = SDL_GetPrefPath(nullptr, g_config.appName);
  } else {
    g_config.cachePath = strdup(g_config.cachePath);
  }

  if (g_config.resourcesPath == nullptr) {
    g_config.resourcesPath = SDL_GetBasePath();
  } else {
    g_config.resourcesPath = strdup(g_config.resourcesPath);
  }

  if (g_config.msaa == 0) {
    g_config.msaa = 1;
  }

  if (g_config.maxTextureAnisotropy == 0) {
    g_config.maxTextureAnisotropy = 16;
  }

  AURORA_ASSERT(window::initialize(), "Error initializing window");

  if (!audio::initialize()) {
    Log.warn("Error initializing audio: {}", SDL_GetError());
  }

  g_sdlCustomEventsStart = SDL_RegisterEvents(2);
  AURORA_ASSERT(g_sdlCustomEventsStart, "Failed to allocate user events: {}", SDL_GetError());
  AURORA_ASSERT(window::initialize_event_watch(), "Error initializing SDL event watch");

#ifdef AURORA_ENABLE_GX
  /* Attempt to create a window using the calling application's desired backend */
  const AuroraBackend requestedBackend = config.desiredBackend;
  AuroraBackend selectedBackend = requestedBackend;
  bool windowCreated = false;
  std::string firstGraphicsError;
  const auto rememberGraphicsError = [&] {
    if (firstGraphicsError.empty() && *SDL_GetError() != '\0') {
      firstGraphicsError = SDL_GetError();
    }
  };

  if (selectedBackend != BACKEND_AUTO) {
    Log.info("Requested graphics backend: {}", backend_name(selectedBackend));
    if (window::create_window(selectedBackend)) {
      if (webgpu::initialize(selectedBackend, config.allowCpuAdapter)) {
        windowCreated = true;
      } else {
        rememberGraphicsError();
        window::destroy_window();
      }
    } else {
      rememberGraphicsError();
    }
    if (!windowCreated) {
      // An unavailable requested backend falls back to the automatic search instead of aborting.
      Log.error("Requested graphics backend {} is unavailable; falling back to automatic selection",
                backend_name(requestedBackend));
    }
  }

  if (!windowCreated) {
    for (const auto backendType : PreferredBackendOrder) {
      selectedBackend = backendType;

      if (!window::create_window(selectedBackend)) {
        rememberGraphicsError();
        continue;
      }

      if (webgpu::initialize(selectedBackend, config.allowCpuAdapter)) {
        windowCreated = true;
        break;
      } else {
        rememberGraphicsError();
        window::destroy_window();
      }
    }
  }

  if (!windowCreated) {
    if (firstGraphicsError.empty()) {
      firstGraphicsError = "No supported graphics backend is available";
    }
    SDL_SetError("%s", firstGraphicsError.c_str());
    Log.error("Graphics initialization failed: {}", firstGraphicsError);
    return {
        .initializationStatus = AURORA_INITIALIZATION_GRAPHICS_UNAVAILABLE,
        .initializationError = SDL_GetError(),
    };
  }
  if (requestedBackend != BACKEND_AUTO && selectedBackend != requestedBackend) {
    Log.error("Graphics backend fallback in effect: requested {}, running on {}",
              backend_name(requestedBackend), backend_name(selectedBackend));
  }

  // Initialize SDL_Renderer for ImGui when we can't use a Dawn backend
  if (webgpu::g_backendType == wgpu::BackendType::Null) {
    AURORA_ASSERT(window::create_renderer(), "Failed to initialize SDL renderer: {}", SDL_GetError());
  }
#else
  AuroraBackend selectedBackend = BACKEND_NULL;
  AURORA_ASSERT(window::create_window(BACKEND_NULL), "Error creating window: {}", SDL_GetError());
  AURORA_ASSERT(window::create_renderer(), "Failed to initialize SDL renderer: {}", SDL_GetError());
#endif

  window::show_window();
  thread::set_current({
      .name = "Main thread",
      .affinity = thread::Affinity::SharedCache,
  });

#ifdef AURORA_ENABLE_GX
  gfx::initialize();
  gx::fifo::init();
  imgui::create_context();
#endif
  const auto size = window::get_window_size();
  Log.info("Using framebuffer size {}x{} scale {}", size.fb_width, size.fb_height, size.scale);
#ifdef AURORA_ENABLE_GX
  if (g_config.imGuiInitCallback != nullptr) {
    g_config.imGuiInitCallback(&size);
  }

  imgui::initialize();

  if (g_config.allowTextureReplacements) {
    g_textureReplacements =
        texture::load_replacement_directory(io::fs_path_from_string(g_config.userPath) / "texture_replacements");
  }
#endif

#ifdef AURORA_ENABLE_RMLUI
  rmlui::initialize(size);
#endif

  g_initialFrame = true;
  g_config.desiredBackend = selectedBackend;
  return {
      .backend = selectedBackend,
      .userPath = g_config.userPath,
      .cachePath = g_config.cachePath,
      .window = window::get_sdl_window(),
      .windowSize = size,
  };
}

void shutdown() noexcept {
  video::shutdown();
  audio::shutdown();
#ifdef AURORA_ENABLE_GX
  gx::fifo::shutdown();
  gfx::render_worker::synchronize();
#ifdef AURORA_ENABLE_RMLUI
  rmlui::shutdown();
#endif
  imgui::shutdown();
  gfx::shutdown();
  webgpu::shutdown();
#endif
  gamepad::shutdown();
  window::shutdown();
}

const AuroraEvent* update() noexcept {
  ZoneScoped;

  if (g_initialFrame) {
    g_initialFrame = false;
    gamepad::initialize();
  }
  const auto* events = window::poll_events();
  gamepad::update();
  return events;
}

bool begin_frame_window() noexcept {
  ZoneScoped;
#ifdef AURORA_ENABLE_GX
  {
    if (!window::is_presentable()) {
      webgpu::release_surface();
      return false;
    }

    if (window::is_paused()) {
      return false;
    }

    if (!g_surface) {
      webgpu::refresh_surface(true);

      if (!g_surface) {
        return false;
      }
    }
  }

  imgui::new_frame(window::get_window_size());
#endif
  return true;
}

bool begin_frame_gx() noexcept {
  ZoneScoped;
#ifdef AURORA_ENABLE_GX
  gx::update();
  window::apply_pending_surface_changes();
  if (!gfx::begin_frame()) {
    return false;
  }

  gx::fifo::begin_frame();
  gx::flush_texture_destructions();
#endif
  return true;
}

bool begin_frame() noexcept { return begin_frame_window() && begin_frame_gx(); }

void end_frame_gx(imgui::DrawData imguiDrawData) noexcept {
  ZoneScoped;
#ifdef AURORA_ENABLE_GX
  gx::flush_texture_destructions();
  gx::fifo::drain();
  gx::fifo::end_frame();
  gx::texture::end_frame();
  gfx::finish();

  // A display copy (the XFB) is what reaches the screen; post-copy EFB draws must not leak in.
  webgpu::TextureWithSampler presentSource = webgpu::present_source();
  gfx::TextureHandle displayCopy;
  if (gx::g_gxState.displayCopyProduced && gx::g_gxState.displayCopyTexture) {
    displayCopy = gx::g_gxState.displayCopyTexture;
    presentSource.texture = displayCopy->texture;
    presentSource.view = displayCopy->sampleTextureView;
    presentSource.size = displayCopy->size;
    presentSource.format = displayCopy->format;
  }
  gx::g_gxState.displayCopyProduced = false;
  const auto surfaceWidth = webgpu::g_graphicsConfig.surfaceConfiguration.width;
  const auto surfaceHeight = webgpu::g_graphicsConfig.surfaceConfiguration.height;
  float presentAspect = 0.f;
  const auto viewport =
      window::get_present_aspect_ratio(presentAspect)
          ? webgpu::calculate_present_viewport_for_aspect(surfaceWidth, surfaceHeight, presentAspect)
          : webgpu::calculate_present_viewport(surfaceWidth, surfaceHeight, presentSource.size.width,
                                               presentSource.size.height);

  wgpu::BindGroup rmlBindGroup;
  bool rmlOverlay = false;
#if AURORA_ENABLE_RMLUI
  if (rmlui::is_initialized()) {
    auto rmlFrame = rmlui::record_frame(viewport);
    rmlBindGroup = std::move(rmlFrame.bindGroup);
    rmlOverlay = rmlFrame.overlay;
  }
#endif

  const auto schedule = gfx::presentation::present_schedule();
  const bool black = video::is_black();
  const float brightness = video::brightness();
  gfx::end_frame([black, brightness, rmlBindGroup = std::move(rmlBindGroup), rmlOverlay, viewport, presentSource,
                  displayCopy = std::move(displayCopy), schedule,
                  imguiDrawData = std::move(imguiDrawData)](
                     wgpu::CommandEncoder& encoder, std::vector<gfx::AfterSubmitCallback> afterSubmitCallbacks,
                     const gfx::InterpolationReplay& replay) {
    const auto presentMode = webgpu::g_graphicsConfig.surfaceConfiguration.presentMode;
    const bool blockingPresent = presentMode == wgpu::PresentMode::Fifo || presentMode == wgpu::PresentMode::FifoRelaxed;
    const auto present = [&](wgpu::CommandEncoder& encoder, bool finalPresent, bool show) {
      if (show && blockingPresent) {
        const wgpu::CommandBufferDescriptor sceneDescriptor{.label = "Scene command buffer"};
        const auto sceneCommands = encoder.Finish(&sceneDescriptor);
        g_queue.Submit(1, &sceneCommands);

        const wgpu::CommandEncoderDescriptor presentDescriptor{.label = "Present encoder"};
        encoder = g_device.CreateCommandEncoder(&presentDescriptor);
      }

      wgpu::Texture currentTexture;
      wgpu::TextureView currentView;
      auto surfaceStatus = wgpu::SurfaceGetCurrentTextureStatus::Error;
      uint64_t surfaceWaitNanos = 0;
      if (show) {
        window::SurfaceLock surfaceLock;

        if (window::is_presentable() && g_surface) {
          ZoneScopedN("Acquire texture");
          wgpu::SurfaceTexture surfaceTexture;
          const uint64_t acquireStart = gfx::presentation::now_nanos();
          g_surface.GetCurrentTexture(&surfaceTexture);
          surfaceWaitNanos = gfx::presentation::now_nanos() - acquireStart;
          surfaceStatus = surfaceTexture.status;

          if (surfaceStatus == wgpu::SurfaceGetCurrentTextureStatus::SuccessOptimal) {
            currentTexture = std::move(surfaceTexture.texture);
            currentView = currentTexture.CreateView();
          }
        }
      }

      const bool canPresent = currentTexture && currentView;

      if (canPresent) {
        wgpu::BindGroup presentBindGroup;

        if (rmlBindGroup && !rmlOverlay) {
          presentBindGroup = rmlBindGroup;
        } else {
          const auto& resampledSource = webgpu::resample_present_source(encoder, viewport, presentSource);
          presentBindGroup = webgpu::create_copy_bind_group(resampledSource);
        }

        {
          const std::array attachments{
              wgpu::RenderPassColorAttachment{
                  .view = currentView,
                  .loadOp = wgpu::LoadOp::Clear,
                  .storeOp = wgpu::StoreOp::Store,
              },
          };

          const wgpu::RenderPassDescriptor renderPassDescriptor{
              .label = "EFB copy render pass",
              .colorAttachmentCount = attachments.size(),
              .colorAttachments = attachments.data(),
              .timestampWrites = webgpu::gpu_prof::pass_writes("Present blit"),
          };

          const auto pass = encoder.BeginRenderPass(&renderPassDescriptor);
          // Copy EFB -> XFB (swapchain)
          if (brightness < 1.0f) {
            pass.SetPipeline(webgpu::g_CopyDimmedPipeline);
            const wgpu::Color blend{brightness, brightness, brightness, 1.0};
            pass.SetBlendConstant(&blend);
          } else {
            pass.SetPipeline(webgpu::g_CopyPipeline);
          }

          pass.SetBindGroup(0, presentBindGroup, 0, nullptr);
          set_present_viewport(pass, viewport, webgpu::g_graphicsConfig.surfaceConfiguration.width,
                               webgpu::g_graphicsConfig.surfaceConfiguration.height);

          if (!black) {
            pass.Draw(3);
          }

          if (rmlBindGroup && rmlOverlay) {
            pass.SetPipeline(webgpu::g_CopyPremultipliedAlphaPipeline);
            pass.SetBindGroup(0, rmlBindGroup, 0, nullptr);
            pass.Draw(3);
          }

          pass.End();
        }

        {
          const std::array attachments{
              wgpu::RenderPassColorAttachment{
                  .view = currentView,
                  .loadOp = wgpu::LoadOp::Load,
                  .storeOp = wgpu::StoreOp::Store,
              },
          };

          const wgpu::RenderPassDescriptor renderPassDescriptor{
              .label = "ImGui render pass",
              .colorAttachmentCount = attachments.size(),
              .colorAttachments = attachments.data(),
              .timestampWrites = webgpu::gpu_prof::pass_writes("ImGui"),
          };

          const auto pass = encoder.BeginRenderPass(&renderPassDescriptor);
          pass.SetViewport(0.f, 0.f, static_cast<float>(webgpu::g_graphicsConfig.surfaceConfiguration.width),
                           static_cast<float>(webgpu::g_graphicsConfig.surfaceConfiguration.height), 0.f, 1.f);
          imgui::render(pass, imguiDrawData);
          pass.End();
        }
      } else if (show) {
        Log.info("Skipping present; window not presentable");
      }

      if (finalPresent) {
        webgpu::gpu_prof::frame_end(encoder);
      }
      const wgpu::CommandBufferDescriptor cmdBufDescriptor{.label = "Redraw command buffer"};
      const auto buffer = encoder.Finish(&cmdBufDescriptor);
      {
        ZoneScopedN("Queue Submit");
        g_queue.Submit(1, &buffer);
      }

      if (finalPresent) {
        webgpu::gpu_prof::after_submit();
      }
      if (canPresent && g_surface) {
        ZoneScopedN("Present");
        wgpu::ConvertibleStatus status = wgpu::Status::Error;
        {
          window::SurfaceLock surfaceLock;

          if (window::is_presentable()) {
            const uint64_t presentStart = gfx::presentation::now_nanos();
            status = g_surface.Present();
            surfaceWaitNanos += gfx::presentation::now_nanos() - presentStart;
          }
        }
        if (blockingPresent) {
          gfx::presentation::report_display_present_wait(surfaceWaitNanos);
        }

        if (status) {
          gfx::after_present();
          gfx::presentation::record_present(false);
        } else {
          Log.warn("Surface present failed");
          webgpu::release_surface();
        }
      } else if (show && g_surface) {
        switch (surfaceStatus) {
        case wgpu::SurfaceGetCurrentTextureStatus::Timeout:
          Log.warn("Surface texture acquisition timed out");
          break;
        case wgpu::SurfaceGetCurrentTextureStatus::SuccessSuboptimal:
        case wgpu::SurfaceGetCurrentTextureStatus::Outdated:
          Log.info("Surface texture is {}, reconfiguring swapchain", magic_enum::enum_name(surfaceStatus));
          window::push_custom_event(window::CustomEvent::RefreshSurface);
          break;
        case wgpu::SurfaceGetCurrentTextureStatus::Lost:
          Log.warn("Surface texture is {}, releasing surface", magic_enum::enum_name(surfaceStatus));
          webgpu::release_surface();
          break;
        case wgpu::SurfaceGetCurrentTextureStatus::Error:
          Log.warn("Surface texture is {}, dropping surface", magic_enum::enum_name(surfaceStatus));
          g_surface = {};
          break;
        default:
          if (!window::is_presentable()) {
            webgpu::release_surface();
          } else {
            Log.error("Failed to get surface texture: {}", magic_enum::enum_name(surfaceStatus));
          }

          break;
        }
      }
    };

    // Interpolated slots replay the frame with blended transforms and present between frames:
    // slot k of N+1 goes out at base + k * interval / (N + 1), the native frame last.
    // A display no faster than the native frame rate cannot show them, so present native frames right away.
    const uint32_t slots =
        gfx::presentation::display_shows_interpolation(schedule.intervalNanos) ? replay.slots : 0;
    const uint32_t groupSize = slots + 1;
    const auto deadline = [&](uint32_t slot) -> uint64_t {
      return slots != 0 && schedule.baseNanos != 0 && schedule.intervalNanos != 0
                 ? schedule.baseNanos + schedule.intervalNanos * slot / groupSize
                 : 0;
    };
    const auto admit = [&](uint64_t presentAt) {
      return !blockingPresent ||
             gfx::presentation::admit_display_present(std::max(presentAt, gfx::presentation::now_nanos()));
    };
    for (uint32_t slot = 0; slot < slots; ++slot) {
      // A slot that would only show after the next one is due adds latency without motion.
      if (const uint64_t next = deadline(slot + 1); next != 0 && gfx::presentation::now_nanos() >= next) {
        continue;
      }
      if (!admit(deadline(slot))) {
        continue;
      }
      const wgpu::CommandEncoderDescriptor slotDescriptor{.label = "Interpolated frame encoder"};
      auto slotEncoder = g_device.CreateCommandEncoder(&slotDescriptor);
      replay.encode(slotEncoder, slot);
      gfx::presentation::wait_until(deadline(slot));
      present(slotEncoder, false, true);
    }
    gfx::presentation::wait_until(deadline(slots));
    present(encoder, true, admit(deadline(slots)));

    for (auto& callback : afterSubmitCallbacks) {
      if (callback) {
        callback();
      }
    }

    gfx::depth_peek::after_submit();

    TracyPlotConfig("aurora: lastVertSize", tracy::PlotFormatType::Memory, false, true, 0);
    TracyPlotConfig("aurora: lastUniformSize", tracy::PlotFormatType::Memory, false, true, 0);
    TracyPlotConfig("aurora: lastIndexSize", tracy::PlotFormatType::Memory, false, true, 0);
    TracyPlotConfig("aurora: lastStorageSize", tracy::PlotFormatType::Memory, false, true, 0);
    TracyPlotConfig("aurora: lastTextureUploadSize", tracy::PlotFormatType::Memory, false, true, 0);

    const auto& stats = gfx::detail::resources().stats;
    TracyPlot("aurora: queuedPipelines", static_cast<int64_t>(stats.queuedPipelines));
    TracyPlot("aurora: createdPipelines", static_cast<int64_t>(stats.createdPipelines));
    TracyPlot("aurora: drawCallCount", static_cast<int64_t>(stats.drawCallCount));
    TracyPlot("aurora: mergedDrawCallCount", static_cast<int64_t>(stats.mergedDrawCallCount));
    TracyPlot("aurora: lastVertSize", static_cast<int64_t>(stats.lastVertSize));
    TracyPlot("aurora: lastUniformSize", static_cast<int64_t>(stats.lastUniformSize));
    TracyPlot("aurora: lastIndexSize", static_cast<int64_t>(stats.lastIndexSize));
    TracyPlot("aurora: lastStorageSize", static_cast<int64_t>(stats.lastStorageSize));
    TracyPlot("aurora: lastTextureUploadSize", static_cast<int64_t>(stats.lastTextureUploadSize));
  });

#endif
}

void end_frame() noexcept {
#ifdef AURORA_ENABLE_GX
  end_frame_gx(imgui::freeze());
#endif
}
} // namespace
} // namespace aurora

struct AuroraImguiFrame {
  aurora::imgui::DrawData drawData;
};

// C API bindings
AuroraInfo aurora_initialize(int argc, char* argv[], const AuroraConfig* config) {
  return aurora::initialize(argc, argv, *config);
}

void aurora_shutdown() { aurora::shutdown(); }

const AuroraEvent* aurora_update() { return aurora::update(); }

bool aurora_begin_frame() { return aurora::begin_frame(); }

void aurora_end_frame() { aurora::end_frame(); }

bool aurora_begin_frame_window() { return aurora::begin_frame_window(); }

bool aurora_begin_frame_gx() { return aurora::begin_frame_gx(); }

AuroraImguiFrame* aurora_end_frame_window() {
#ifdef AURORA_ENABLE_GX
  return new AuroraImguiFrame{aurora::imgui::freeze()};
#else
  return nullptr;
#endif
}

void aurora_release_imgui_frame(AuroraImguiFrame* frame) { delete frame; }

void aurora_end_frame_gx(AuroraImguiFrame* frame) {
  std::unique_ptr<AuroraImguiFrame> owned{frame};
  aurora::end_frame_gx(owned ? std::move(owned->drawData) : aurora::imgui::DrawData{});
}

SDL_Window* aurora_get_window() { return aurora::window::get_sdl_window(); }

AuroraWindowSize aurora_get_window_size() {
  return aurora::window::get_sdl_window() != nullptr ? aurora::window::get_window_size() : AuroraWindowSize{};
}

AuroraBackend aurora_get_backend() { return aurora::g_config.desiredBackend; }

const AuroraBackend* aurora_get_available_backends(size_t* count) {
  if (count != nullptr) {
    *count = aurora::PreferredBackendOrder.size();
  }

  return aurora::PreferredBackendOrder.data();
}

void aurora_set_log_level(AuroraLogLevel level) { aurora::g_config.logLevel = level; }

void aurora_set_pause_on_focus_lost(bool value) { aurora::g_config.pauseOnFocusLost = value; }

void aurora_set_display_mode(AuroraDisplayMode mode) { aurora::window::set_display_mode(mode); }

AuroraDisplayMode aurora_get_display_mode() { return aurora::window::get_display_mode(); }

double aurora_get_display_refresh_rate() {
  const uint64_t period = aurora::window::refresh_period_nanos();
  return period != 0 ? 1e9 / static_cast<double>(period) : 0.0;
}

#if !defined(__APPLE__)
void aurora_set_metalfx_spatial(bool) {}
bool aurora_get_metalfx_spatial() { return false; }
bool aurora_is_metalfx_spatial_supported() { return false; }
AuroraMetalFXStatus aurora_get_metalfx_status() { return AURORA_METALFX_UNSUPPORTED; }
#endif

void aurora_set_background_input(bool value) {
  aurora::g_config.allowJoystickBackgroundEvents = value;
  aurora::window::set_background_input(value);
}

void aurora_set_resampler(AuroraSampler sampler) {
#ifdef AURORA_ENABLE_GX
  aurora::webgpu::set_resampler(sampler);
#else
  (void)sampler;
#endif
}

void aurora_set_timescale(float scale) { aurora::time::set_scale(scale); }

float aurora_get_timescale() { return aurora::time::scale(); }
