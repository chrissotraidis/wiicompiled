#pragma once

#include "frame_packet.hpp"

namespace aurora::gfx {

bool bind_pipeline(PipelineRef ref, const wgpu::RenderPassEncoder& pass);
void bind_pipeline(const wgpu::RenderPipeline& pipeline, const wgpu::RenderPassEncoder& pass);

namespace detail {
void encode_op(wgpu::CommandEncoder& encoder, FramePacket& frame, const FrameOp& op);
void encode_full_staging_copy(wgpu::CommandEncoder& cmd, FramePacket& frame);
void encode_replay_op(wgpu::CommandEncoder& cmd, FramePacket& frame, const FrameOp& op);
}

} // namespace aurora::gfx
