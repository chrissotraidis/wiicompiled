#pragma once

#include "common.hpp"

#include <functional>

namespace aurora::gfx::clear {
struct PipelineConfig;
} // namespace aurora::gfx::clear

namespace aurora::gx {
struct PipelineConfig;
} // namespace aurora::gx

namespace aurora::gfx {

using NewPipelineCallback = std::function<wgpu::RenderPipeline()>;

void initialize_pipeline_cache();
void shutdown_pipeline_cache();
void begin_pipeline_frame();
void end_pipeline_frame();
void set_skip_unready_pipelines(bool enabled) noexcept;
bool skip_unready_pipelines() noexcept;
void set_race_copy_skip(bool raceActive) noexcept;
bool race_copy_skip_active() noexcept;
uint32_t queued_pipeline_count() noexcept;
// Changes when a course scene changes so GX's draw memos can record reused pipelines.
uint32_t pipeline_scene_generation() noexcept;

template <typename Config>
PipelineRef find_pipeline(ShaderType type, const Config& config, NewPipelineCallback&& cb);

bool wait_pipeline(PipelineRef ref, wgpu::RenderPipeline& pipeline);
bool try_pipeline(PipelineRef ref, wgpu::RenderPipeline& pipeline);
// Waits indefinitely for a draw about to be committed to a persistent texture with no re-issue
// path. Separate from wait_pipeline so skip-unready mode never becomes blocking for other passes.
bool wait_pipeline_for_persistent_pass(PipelineRef ref, wgpu::RenderPipeline& pipeline);

} // namespace aurora::gfx
