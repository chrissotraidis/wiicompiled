#pragma once

#include "gx.hpp"

#include <vector>

// Adreno character-geometry workaround (KartPad issues #104/#193/#316 family).
//
// Draws whose vertices carry their own position-matrix index (PNMTXIDX direct)
// are the draws that break on Qualcomm drivers. In the normal path their vertex
// shader reads a 1-byte matrix index and 1-byte array indices from an odd-stride,
// unaligned vertex stream and then performs a second, dependent storage-buffer
// read into the attribute arrays. When enabled, this module resolves every
// indexed attribute on the CPU and rewrites each vertex as 4-byte-aligned direct
// data (matrix indices in zero-padded 32-bit slots; position, normal and
// texcoords as big-endian f32; colors as their original bytes), so the shader
// performs only aligned, non-dependent reads from one buffer. The matrix palette
// lookup itself is unchanged.
namespace aurora::gx::kartpad_repack {

// Process-constant decision. KARTPAD_RENDERER_VERTEX_REPACK=1/0 forces it on/off;
// on Android the debug.kartpad.vertex_repack system property (1/0) does the same
// (settable with `adb shell setprop`); otherwise it is on for Qualcomm adapters.
bool enabled() noexcept;

// Whether the current GX state and primitive take the repacked path.
inline bool applies(GXPrimitive prim) noexcept {
  return g_gxState.vtxDesc[GX_VA_PNMTXIDX] == GX_DIRECT && prim != GX_LINES && prim != GX_LINESTRIP &&
         prim != GX_POINTS && enabled();
}

// Rewrites a source vertex layout into the repacked layout in place; returns the new stride.
u8 repacked_layout(std::array<AttrConfig, MaxVtxAttr>& attrs) noexcept;

// Repacks `count` vertices of format `fmt` (source stride `srcStride`) using the live
// attribute arrays. The returned buffer is reused by the next call on this thread.
const std::vector<u8>& repack(GXVtxFmt fmt, const u8* vertices, u16 count, u32 srcStride);

// Layout-explicit entry used by repack() and host checks.
void repack_with_layout(const std::array<AttrConfig, MaxVtxAttr>& src, u32 srcStride,
                        const std::array<AttrConfig, MaxVtxAttr>& dst, u32 dstStride, const u8* vertices,
                        u16 count, const std::array<AttrArray, MaxVtxAttr>& arrays, std::vector<u8>& out);

// Rate-limited "repack active" evidence line, emitted when a repacked pipeline is first resolved.
void note_pipeline(u64 sourceHash, u64 repackedHash, u32 srcStride, u32 dstStride) noexcept;

} // namespace aurora::gx::kartpad_repack
