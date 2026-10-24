#ifndef AURORA_AURORA_H
#define AURORA_AURORA_H

#ifdef __cplusplus
#include <cstddef>
#include <cstdint>

extern "C" {
#else
#include "stdbool.h"
#include "stddef.h"
#include "stdint.h"
#endif

typedef enum {
  SAMPLER_BILINEAR,
  SAMPLER_AREA,
} AuroraSampler;

typedef enum {
  BACKEND_AUTO,
  BACKEND_D3D11,
  BACKEND_D3D12,
  BACKEND_METAL,
  BACKEND_VULKAN,
  BACKEND_OPENGL,
  BACKEND_OPENGLES,
  BACKEND_WEBGPU,
  BACKEND_NULL,
} AuroraBackend;

typedef enum {
  LOG_DEBUG,
  LOG_INFO,
  LOG_WARNING,
  LOG_ERROR,
  LOG_FATAL,
} AuroraLogLevel;

typedef enum {
  AURORA_DISPLAY_MODE_WINDOWED,
  AURORA_DISPLAY_MODE_BORDERLESS,
  AURORA_DISPLAY_MODE_EXCLUSIVE,
} AuroraDisplayMode;

typedef struct {
  int32_t x;
  int32_t y;
} AuroraWindowPos;

typedef struct {
  uint32_t width;
  uint32_t height;

  /**
   * Width of the main GX framebuffer.
   */
  uint32_t fb_width;

  /**
   * Height of the main GX framebuffer.
   */
  uint32_t fb_height;

  /**
   * The size of the framebuffer used to present to the operating system.
   * May differ from fb_width if Aurora is instructed to force an aspect ratio or resolution configuration.
   */
  uint32_t native_fb_width;

  /**
   * The size of the framebuffer used to present to the operating system.
   * May differ from fb_height if Aurora is instructed to force an aspect ratio or resolution configuration.
   */
  uint32_t native_fb_height;
  float scale;
} AuroraWindowSize;

typedef struct SDL_Window SDL_Window;
typedef struct AuroraEvent AuroraEvent;

typedef void (*AuroraLogCallback)(AuroraLogLevel level, const char* module, const char* message, unsigned int len);
typedef void (*AuroraImGuiInitCallback)(const AuroraWindowSize* size);

#define MEM1_DEFAULT_SIZE (24 * 1024 * 1024)
#define MEM2_DEFAULT_SIZE (64 * 1024 * 1024)
#define ARAM_DEFAULT_SIZE (16 * 1024 * 1024)

typedef struct {
  const char* appName;
  const char* userPath;
  const char* cachePath;
  const char* resourcesPath;
  AuroraBackend desiredBackend;
  uint32_t msaa;
  uint16_t maxTextureAnisotropy;
  bool vsync;
  bool startFullscreen;
  bool allowJoystickBackgroundEvents;
  bool pauseOnFocusLost;
  bool allowTextureDumps;
  bool allowTextureReplacements;
  bool allowCpuAdapter;
  bool disableCopyFilter;
  // When false, Aurora centers the first window. When true, windowPosX/Y are restored verbatim,
  // including negative coordinates on monitors left of or above the primary display.
  bool hasWindowPosition;
  int32_t windowPosX;
  int32_t windowPosY;
  uint32_t windowWidth;
  uint32_t windowHeight;
  void* iconRGBA8;
  uint32_t iconWidth;
  uint32_t iconHeight;
  AuroraLogCallback logCallback;
  AuroraLogLevel logLevel;
  AuroraImGuiInitCallback imGuiInitCallback;

  /*
   * The size of the GameCube's main memory, or MEM1 on the Wii.
   * Note that it will not be allocated at the exact 0x80000000 address, as that cannot be guaranteed.
   * This can be set to 0 to disable allocating this region.
   */
  uint32_t mem1Size;

  /*
   * The size of the GameCube's ARAM, or MEM2 on the Wii.
   * This can be set to 0 to disable allocating this region.
   */
  uint32_t mem2Size;
} AuroraConfig;

typedef enum {
  AURORA_INITIALIZATION_SUCCESS = 0,
  AURORA_INITIALIZATION_GRAPHICS_UNAVAILABLE = 1,
} AuroraInitializationStatus;

typedef struct {
  AuroraBackend backend;
  const char* userPath;
  const char* cachePath;
  SDL_Window* window;
  AuroraWindowSize windowSize;
  AuroraInitializationStatus initializationStatus;
  // On failure, owned by SDL on the calling thread. Copy before another SDL call.
  const char* initializationError;
} AuroraInfo;

AuroraInfo aurora_initialize(int argc, char* argv[], const AuroraConfig* config);
void aurora_shutdown();
const AuroraEvent* aurora_update();
bool aurora_begin_frame();
void aurora_end_frame();

// aurora_begin_frame/aurora_end_frame split for a GX producer running on its own thread. The
// window halves stay on the thread that owns the window and ImGui; the GX halves run on the
// producer thread.
typedef struct AuroraImguiFrame AuroraImguiFrame;
bool aurora_begin_frame_window();
bool aurora_begin_frame_gx();
AuroraImguiFrame* aurora_end_frame_window();
// Takes ownership of frame.
void aurora_end_frame_gx(AuroraImguiFrame* frame);
// Frees a frame that will not be ended.
void aurora_release_imgui_frame(AuroraImguiFrame* frame);

typedef void (*AuroraFrameWorkerWaitCallback)();
// Called from the producer thread at bounded intervals while Aurora blocks on rendering.
// The callback must not enter Aurora.
void aurora_set_frame_worker_wait_callback(AuroraFrameWorkerWaitCallback callback);
// Waits until every frame handed to the renderer has been encoded.
void aurora_wait_for_frame_worker();
bool aurora_wait_for_frame_worker_for(uint32_t timeoutMicros);
// Absolute schedule for the next frame, on steady_clock: baseNanos anchors the group and
// intervalNanos is the period, so slot k of N+1 presents at base + k*interval/(N+1). Zeros clear it.
void aurora_set_present_schedule(uint64_t baseNanos, uint64_t intervalNanos);
// Reports whether the frame about to be ended met its display boundary. Interpolation sizes its
// slot group from this, backing off after misses. Only paced presents may report.
void aurora_report_producer_paced(bool paced);

void aurora_set_log_level(AuroraLogLevel level);
void aurora_set_pause_on_focus_lost(bool value);
void aurora_set_background_input(bool value);
void aurora_set_display_mode(AuroraDisplayMode mode);
AuroraDisplayMode aurora_get_display_mode();
// Refresh rate of the display showing the window in Hz, or 0 when unknown.
double aurora_get_display_refresh_rate();
// False when the display is too slow to show interpolated frames, which are then dropped.
bool aurora_display_shows_interpolation();

typedef enum {
  AURORA_METALFX_DISABLED,
  AURORA_METALFX_UNSUPPORTED,
  AURORA_METALFX_NOT_UPSCALING,
  AURORA_METALFX_ACTIVE,
  AURORA_METALFX_ERROR,
} AuroraMetalFXStatus;

// MetalFX only applies when both source dimensions are smaller than the aspect-fitted output.
void aurora_set_metalfx_spatial(bool enabled);
bool aurora_get_metalfx_spatial();
bool aurora_is_metalfx_spatial_supported();
AuroraMetalFXStatus aurora_get_metalfx_status();
void aurora_set_resampler(AuroraSampler sampler);
/** Sets the clock timescale. Default 1.0f. 0.0f is paused. Range 0.0f-16.0f. */
void aurora_set_timescale(float scale);

SDL_Window* aurora_get_window();
AuroraWindowSize aurora_get_window_size();

AuroraBackend aurora_get_backend();
const AuroraBackend* aurora_get_available_backends(size_t* count);
float aurora_get_timescale();

#ifdef __cplusplus
}
#endif

#endif
