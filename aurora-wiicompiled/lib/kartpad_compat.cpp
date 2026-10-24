// KartPad runtime compatibility with the classic Aurora's API.
//
// KartPad builds one runtime against two Aurora versions. These entry points exist in the
// classic Aurora; here they are either unnecessary or not ported yet.
#include <aurora/gfx.h>
#include <dolphin/gx.h>

// This Aurora decodes indexed attributes from the vertex descriptor itself.
void GXSetSourceVtxDesc(GXAttr, GXAttrType) {}

// Course pipeline recipes, launch prewarm and race copy skipping are classic-Aurora
// optimizations that are not ported yet. Without them, unready pipelines compile on demand.
void aurora_set_race_copy_skip(bool) {}
bool aurora_get_pipeline_prewarm_progress(uint32_t* remaining, uint32_t* total) {
  if (remaining != nullptr) {
    *remaining = 0;
  }
  if (total != nullptr) {
    *total = 0;
  }
  return false;
}
void aurora_set_pipeline_scene(uint64_t) {}

#if defined(__ANDROID__)
extern "C" void KartPadAndroidLogMetric(const char*, const char*, ...);
// Called from the periodic telemetry on the game thread. The classic Aurora reports its copy
// texture pool here; this renderer has no such pool, so it reports its draw and upload counters.
extern "C" void KartPadLogGpuResourceMetrics() {
  const AuroraStats* stats = aurora_get_stats();
  if (stats == nullptr) {
    return;
  }
  KartPadAndroidLogMetric("KartPadGpuResources",
                          "renderer=aurora-wiicompiled pipelines=%u queued_pipelines=%u draws=%u "
                          "merged_draws=%u texture_upload_bytes=%u",
                          stats->createdPipelines, stats->queuedPipelines, stats->drawCallCount,
                          stats->mergedDrawCallCount, stats->lastTextureUploadSize);
}
#endif
