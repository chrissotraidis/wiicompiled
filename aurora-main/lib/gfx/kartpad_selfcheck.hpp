#pragma once

// Once-per-session draw self-check (KartPad Android, #104/#301/#304 family).
//
// Some Android GPU drivers draw characters wrong while the same vertices repacked on the CPU
// draw correctly. To tell from a player's own log whether that is happening, one real skinned
// draw per session is drawn twice off screen: with the vertex layout the game used and with the
// other one (shader fetch versus CPU repack), each from the same uniforms and textures. Both
// images are read back and compared. When the draw can use it, a third copy keeps the game's layout
// but looks the bone matrices up with the constant (switch) lookup instead of indexing. One line is
// logged:
//   KartPad draw self-check: ... result=match|mismatch|indexing|inconclusive
// (mismatch: the vertex layout changes the picture; indexing: the matrix lookup does.)
// It never changes what is shown. KARTPAD_DRAW_SELFCHECK=0 turns it off;
// Tests: KARTPAD_DRAW_SELFCHECK_BREAK or the Android property debug.kartpad.selfcheck_break set to 1
// corrupts the other-layout copy (mismatch must be reported); 2 leaves both layout copies empty
// (indexing must be reported when the constant-lookup copy draws).

#include "../gx/pipeline.hpp"

namespace aurora::gfx {
struct ClipRect;
}

namespace aurora::gfx::kartpad_selfcheck {

// Character draws smaller than this are pieces (eyes, buckles) that cover too few pixels to judge.
inline constexpr uint32_t MinVertices = 64;

// Record side (game thread). True when the next large skinned draw should be checked; such a
// draw is then kept out of merging so the whole model is compared.
bool wanted() noexcept;
// Stores the draws and places a marker right after the real draw in this frame's commands.
// `constant` is the optional constant-lookup copy.
void record(const gx::DrawData& main, const gx::DrawData& twin, const gx::DrawData* constant, bool mainRepacked,
            uint32_t vertices);
// Test hook: whether the twin's vertices should be corrupted.
bool break_twin() noexcept;
// Logs (once) why a wanted draw could not be checked; the next eligible draw tries again.
void skipped(const char* reason) noexcept;

// Render side. Called when the marker is replayed in a native (not interpolated) frame.
void capture(const Viewport* viewport, const ClipRect* scissor, wgpu::Extent3D targetSize, uint32_t samples);
// After the pass that held the marker: draws both copies off screen and queues the readback.
void encode(const wgpu::CommandEncoder& cmd, const wgpu::BindGroup& staticBindGroup);
// After queue submission: maps the readback buffers; the result is logged when both arrive.
void after_submit() noexcept;

} // namespace aurora::gfx::kartpad_selfcheck
