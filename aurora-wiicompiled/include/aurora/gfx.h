#ifndef AURORA_GFX_H
#define AURORA_GFX_H

#ifdef __cplusplus
#include <cstddef>
#include <cstdint>

extern "C" {
#else
#include "stddef.h"
#include "stdint.h"
#endif

#if !defined(NDEBUG) && !defined(AURORA_GFX_DEBUG_GROUPS)
#define AURORA_GFX_DEBUG_GROUPS
#endif

void push_debug_group(const char* label);
void pop_debug_group();

typedef struct {
  uint32_t queuedPipelines;
  uint32_t createdPipelines;
  uint32_t drawCallCount;
  uint32_t mergedDrawCallCount;
  uint32_t lastVertSize;
  uint32_t lastUniformSize;
  uint32_t lastIndexSize;
  uint32_t lastStorageSize;
  uint32_t lastTextureUploadSize;
  uint32_t presentedFrameCount;
  uint32_t interpolatedFrameCount;
} AuroraStats;

typedef struct {
  uint64_t totalPresentCount;
  uint32_t sampleCount;
  double framesPerSecond;
  double averageFrameTimeMs;
  double p50FrameTimeMs;
  double p95FrameTimeMs;
  double p99FrameTimeMs;
  double worstFrameTimeMs;
  double jitterMs;
  // framesPerSecond with duplicated presentation slots scaled out: the rate of frames that
  // carried new motion.
  double effectiveFramesPerSecond;
} AuroraPresentTiming;

const AuroraStats* aurora_get_stats();
float aurora_get_fps();
void aurora_get_present_timing(AuroraPresentTiming* timing);

// Interpolation health: the per-frame fields describe the last frame, the counters accumulate
// since it was configured.
typedef struct {
  uint32_t targetFps;
  uint32_t targetSamples;
  uint32_t activeSamples;
  uint32_t candidates;
  uint32_t matchable;
  uint32_t matches;
  uint32_t eligible;
  uint32_t replaySafe;
  uint64_t framesSealed;
  uint64_t framesLowMatch;
  uint64_t framesReplayUnsafe;
  uint64_t slotReductions;
  uint64_t lateSealDrops;
} AuroraFrameInterpolationDiagnostics;

void aurora_get_frame_interpolation_diagnostics(AuroraFrameInterpolationDiagnostics* diagnostics);

// Generates transform-interpolated perspective frames between consecutive 60 Hz logical frames.
// Supported targets are 0 (off), 120, 180 and 240. Guest simulation and VI timing are unchanged.
void aurora_set_frame_interpolation_fps(uint32_t targetFps);
uint32_t aurora_get_frame_interpolation_fps();

// When enabled, draws whose pipeline is still compiling are skipped instead of stalling.
void aurora_set_skip_unready_pipelines(bool enabled);
bool aurora_get_skip_unready_pipelines();
// KartPad: called on course/menu archive loads; races may let recurring EFB copies skip
// unready draws.
void aurora_set_race_copy_skip(bool raceActive);
uint32_t aurora_get_queued_pipeline_count();
// KartPad: true while launch prewarm runs; reports recipes remaining of those queued at launch.
bool aurora_get_pipeline_prewarm_progress(uint32_t* remaining, uint32_t* total);
// KartPad: identifies the scene (for example a course archive) that is starting to load.
// 0 clears.
void aurora_set_pipeline_scene(uint64_t scene);

// Controls whether display copies bypass the vertical copy filter.
void aurora_set_disable_copy_filter(bool disabled);
bool aurora_get_disable_copy_filter();

// Guest-RAM write tracking. `generation` changes whenever guest RAM covering a host range was
// written (or returns AURORA_GUEST_WRITE_UNTRACKED); `notify` reports writes aurora made itself.
#define AURORA_GUEST_WRITE_UNTRACKED UINT64_MAX
typedef uint64_t (*AuroraGuestWriteGenerationCallback)(const void* hostPtr, size_t size);
typedef void (*AuroraGuestWriteNotifyCallback)(const void* hostPtr, size_t size);
void aurora_set_guest_write_hooks(AuroraGuestWriteGenerationCallback generation,
                                  AuroraGuestWriteNotifyCallback notify);

void aurora_enable_vsync(bool enabled);

#ifdef __cplusplus
}
#endif

#endif
