#include "gx.hpp"
#include "__gx.h"

#include "../../gfx/texture.hpp"
#include "../../gfx/presentation.hpp"
#include "../../gfx/readback/copy_layout.hpp"
#include "../../gfx/recording.hpp"
#include "../../window.hpp"
#include "../../webgpu/gpu.hpp"
#include "../../gx/texture.hpp"
#include "../vi/vi_internal.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>

namespace {
// Color copies wait for compiling pipelines so bakes the guest keeps (icons, thumbnails) are never
// missing draws. Only large copies that recur every frame, like post-processing, may skip.
constexpr u32 CopyWarmupFrames = 8;
constexpr u32 SmallCopyAreaDivisor = 4;
constexpr u32 ProbeMaxBytes = 256;
constexpr size_t MaxProbes = 32;

u16 sTexCopyWidth = 0;
u16 sTexCopyHeight = 0;
GXTexFmt sTexCopyFmt = GX_TF_I8;
std::unordered_map<void*, AuroraReadbackStream> sProbeStreams;

// Probe-sized depth copies (e.g. a lens-flare occlusion test) are read back into guest RAM a few frames late.
void read_back_probe(void* dest) {
  const u32 size = aurora::gfx::readback::byte_size(sTexCopyFmt, sTexCopyWidth, sTexCopyHeight);
  if (dest == nullptr || !aurora::gx::is_depth_format(sTexCopyFmt) || size == 0 || size > ProbeMaxBytes) {
    return;
  }
  auto it = sProbeStreams.find(dest);
  if (it == sProbeStreams.end()) {
    if (sProbeStreams.size() >= MaxProbes) {
      return;
    }
    const AuroraReadbackStream stream = AuroraReadbackCreateStream();
    if (stream == 0) {
      return;
    }
    it = sProbeStreams.emplace(dest, stream).first;
    // Far Z until the first result arrives, so a depth probe reads as unobstructed.
    std::memset(dest, 0xFF, size);
    aurora::gfx::presentation::notify_guest_write(dest, size);
  }
  AuroraReadbackDest result{.key = 0, .dest = dest, .size = size};
  if (AuroraReadbackGet(it->second, &result, 1)) {
    aurora::gfx::presentation::notify_guest_write(dest, size);
  }
  const AuroraReadbackCopySrc src{.key = 0, .src = dest};
  AuroraReadbackCaptureCopies(it->second, &src, 1);
}

aurora::Vec2<uint32_t> scale_copy_dst(u32 logicalWidth, u32 logicalHeight) {
  if (g_gxState.viewportPolicy == AURORA_VIEWPORT_NATIVE) {
    return {logicalWidth, logicalHeight};
  }

  const auto [logicalFbWidth, logicalFbHeight] = aurora::gx::logical_fb_size();
  const auto [targetWidth, targetHeight] = aurora::gfx::get_render_target_size();
  if (logicalFbWidth == 0 || logicalFbHeight == 0 || targetWidth == 0 || targetHeight == 0) {
    return {logicalWidth, logicalHeight};
  }

  const float scaleX = static_cast<float>(targetWidth) / static_cast<float>(logicalFbWidth);
  const float scaleY = static_cast<float>(targetHeight) / static_cast<float>(logicalFbHeight);
  const auto scaledWidth = std::max<u32>(static_cast<u32>(std::lround(static_cast<float>(logicalWidth) * scaleX)), 1);
  const auto scaledHeight = std::max<u32>(static_cast<u32>(std::lround(static_cast<float>(logicalHeight) * scaleY)), 1);
  return {scaledWidth, scaledHeight};
}
} // namespace

namespace {
// Vertical copy filter taps from BP 0x53/0x54, folded to (prev, current, next) rows.
std::array<u32, 3> copy_filter_taps() noexcept {
  const u32 f0 = g_gxState.bpRegCache[0x53];
  const u32 f1 = g_gxState.bpRegCache[0x54];
  if (!g_gxState.bpRegValid.test(0x53) || !g_gxState.bpRegValid.test(0x54)) {
    return {0, 64, 0};
  }
  const auto tap = [](u32 value, u32 shift) { return (value >> shift) & 0x3fu; };
  return {
      tap(f0, 0) + tap(f0, 6),
      tap(f0, 12) + tap(f0, 18) + tap(f1, 0),
      tap(f1, 6) + tap(f1, 12),
  };
}

u16 num_xfb_lines(u16 efbHeight, u32 iScale) noexcept {
  if (efbHeight == 0 || iScale == 0) {
    return efbHeight;
  }
  const u32 count = static_cast<u32>(efbHeight - 1u) * 0x100u;
  u32 realHeight = count / iScale + 1u;
  u32 reducedScale = iScale;
  if (reducedScale > 0x80u && reducedScale < 0x100u) {
    while ((reducedScale & 1u) == 0u) {
      reducedScale >>= 1;
    }
    if (efbHeight % reducedScale == 0u) {
      ++realHeight;
    }
  }
  return static_cast<u16>(std::min<u32>(realHeight, 0x400u));
}

u32 y_scale_to_integer(f32 yScale) noexcept { return yScale > 0.f ? static_cast<u32>(256.f / yScale) & 0x1ffu : 256u; }

// Producer-side mirror of the display copy source height, for GXSetDispCopyYScale's return value.
u16 sDispCopySrcHeight = 0;
} // namespace

namespace aurora::gx {
void copy_disp(GXBool clear, u32 copyControl) noexcept {
  const u32 src0 = g_gxState.bpRegCache[0x49];
  const u32 src1 = g_gxState.bpRegCache[0x4A];
  const gfx::ClipRect src{
      .x = static_cast<int32_t>(src0 & 0x3ffu),
      .y = static_cast<int32_t>((src0 >> 10) & 0x3ffu),
      .width = static_cast<int32_t>((src1 & 0x3ffu) + 1),
      .height = static_cast<int32_t>(((src1 >> 10) & 0x3ffu) + 1),
  };
  const u32 stride = g_gxState.bpRegCache[0x4D] & 0x3ffu;
  const u32 logicalDstWidth = stride != 0 ? stride * 16u : static_cast<u32>(src.width);
  const u32 iScale = g_gxState.bpRegValid.test(0x4E) ? g_gxState.bpRegCache[0x4E] & 0x1ffu : 256u;
  const u32 logicalDstHeight = num_xfb_lines(static_cast<u16>(src.height), iScale);
  const auto rect = map_logical_scissor(src);
  const auto [dstWidth, dstHeight] = scale_copy_dst(std::max<u32>(logicalDstWidth, 1), std::max<u32>(logicalDstHeight, 1));

  auto& texture = g_gxState.displayCopyTexture;
  if (!texture || texture->size.width != dstWidth || texture->size.height != dstHeight) {
    texture = gfx::new_render_texture(dstWidth, dstHeight, GX_TF_RGBA8, "Display Copy");
  }

  auto filter = copy_filter_taps();
  if (g_config.disableCopyFilter) {
    filter = {0, filter[0] + filter[1] + filter[2], 0};
  }
  const auto clearColor = clear && g_gxState.colorUpdate;
  const auto clearAlpha = clear && g_gxState.alphaUpdate && efb_has_alpha(g_gxState.pixelFmt);
  const auto clearDepth = clear && g_gxState.depthUpdate;
  gfx::resolve_pass_into(texture, rect, clearColor, clearAlpha, clearDepth, g_gxState.clearColor, clear_depth_value(),
                         GX_TF_RGBA8, g_gxState.pixelFmt,
                         {
                             .copyFilter = filter,
                             .copyFilterRowStride = static_cast<float>(rect.height) / std::max(src.height, 1),
                             .clampTop = (copyControl & GX_CLAMP_TOP) != 0,
                             .clampBottom = (copyControl & GX_CLAMP_BOTTOM) != 0,
                         });
  g_gxState.displayCopyProduced = true;
}

void copy_tex(const void* dest, GXBool clear, u32 copyControl) noexcept {
  const auto rect = map_logical_scissor(g_gxState.texCopySrc);
  const auto [dstWidth, dstHeight] = scale_copy_dst(g_gxState.texCopyDstWidth, g_gxState.texCopyDstHeight);
  const auto texCopyFmt = g_gxState.texCopyFmt;
  const auto copy = texture::resolve_copy_texture({
      .dest = dest,
      .width = dstWidth,
      .height = dstHeight,
      .format = texCopyFmt,
  });
  auto& current = g_gxState.copyTextures[dest];
  current.width = g_gxState.texCopyDstWidth;
  current.height = g_gxState.texCopyDstHeight;
  current.pixelFormat = g_gxState.pixelFmt;

  const auto clearColor = clear && g_gxState.colorUpdate;
  const auto clearAlpha = clear && g_gxState.alphaUpdate && efb_has_alpha(g_gxState.pixelFmt);
  const auto clearDepth = clear && g_gxState.depthUpdate;
  const auto src = g_gxState.texCopySrc;
  const auto [fbWidth, fbHeight] = logical_fb_size();
  const bool smallCopy = static_cast<u64>(std::max(src.width, 0)) * std::max(src.height, 0) * SmallCopyAreaDivisor <=
                         static_cast<u64>(fbWidth) * fbHeight;
  const bool persistent =
      !is_depth_format(texCopyFmt) && (smallCopy || copy.consecutiveFrames < CopyWarmupFrames);
  gfx::resolve_pass_into(copy.handle, rect, clearColor, clearAlpha, clearDepth, g_gxState.clearColor,
                         clear_depth_value(), texCopyFmt, g_gxState.pixelFmt,
                         {
                             .copyFilter = copy_filter_taps(),
                             .copyFilterRowStride = static_cast<float>(rect.height) / std::max(src.height, 1),
                             .clampTop = (copyControl & GX_CLAMP_TOP) != 0,
                             .clampBottom = (copyControl & GX_CLAMP_BOTTOM) != 0,
                             .persistent = persistent,
                         });
}
} // namespace aurora::gx

extern "C" {
GXRenderModeObj GXNtsc480IntDf = {
    VI_TVMODE_NTSC_INT,
    640,
    480,
    480,
    40,
    0,
    640,
    480,
    VI_XFBMODE_DF,
    0,
    0,
    {6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6},
    {8, 8, 10, 12, 10, 8, 8},
};
GXRenderModeObj GXNtsc480Int = {
    VI_TVMODE_NTSC_INT,
    640,
    480,
    480,
    40,
    0,
    640,
    480,
    VI_XFBMODE_DF,
    0,
    0,
    {6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6},
    {0, 0, 21, 22, 21, 0, 0},
};
GXRenderModeObj GXPal528IntDf = {
    VI_TVMODE_PAL_INT,
    704,
    528,
    480,
    40,
    0,
    640,
    480,
    VI_XFBMODE_DF,
    0,
    0,
    {6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6},
    {8, 8, 10, 12, 10, 8, 8},
};
GXRenderModeObj GXMpal480IntDf = {
    VI_TVMODE_PAL_INT,
    640,
    480,
    480,
    40,
    0,
    640,
    480,
    VI_XFBMODE_DF,
    0,
    0,
    {6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6},
    {8, 8, 10, 12, 10, 8, 8},
};

void GXAdjustForOverscan(GXRenderModeObj* rmin, GXRenderModeObj* rmout, u16 hor, u16 ver) {
  const GXRenderModeObj in = *rmin;
  const u16 hor2 = hor * 2;
  const u16 ver2 = ver * 2;
  const u32 efbHeight = in.efbHeight;

  *rmout = in;
  rmout->fbWidth = in.fbWidth - hor2;
  rmout->efbHeight = efbHeight - (ver2 * efbHeight) / in.xfbHeight;

  if (in.xFBmode == VI_XFBMODE_SF && (in.viTVmode & 2) != 2) {
    rmout->xfbHeight = in.xfbHeight - ver;
  } else {
    rmout->xfbHeight = in.xfbHeight - ver2;
  }

  rmout->viWidth = in.viWidth - hor2;
  rmout->viHeight = in.viHeight - ver2;
  rmout->viXOrigin = in.viXOrigin + hor;
  rmout->viYOrigin = in.viYOrigin + ver;
}

void GXSetDispCopySrc(u16 left, u16 top, u16 wd, u16 ht) {
  sDispCopySrcHeight = ht;
  GX_WRITE_RAS_REG(0x49000000u | ((static_cast<u32>(top) & 0x3ffu) << 10) | (static_cast<u32>(left) & 0x3ffu));
  GX_WRITE_RAS_REG(0x4A000000u | (((static_cast<u32>(ht) - 1u) & 0x3ffu) << 10) |
                   ((static_cast<u32>(wd) - 1u) & 0x3ffu));
  __gx->bpSent = 0;
}

void GXSetTexCopySrc(u16 left, u16 top, u16 wd, u16 ht) {
  GX_WRITE_AURORA(GX_AURORA_LOAD_COPY_SRC);
  GX_WRITE_U32(left);
  GX_WRITE_U32(top);
  GX_WRITE_U32(wd);
  GX_WRITE_U32(ht);
}

void GXSetDispCopyDst(u16 wd, u16 ht) {
  (void)ht;
  GX_WRITE_RAS_REG(0x4D000000u | ((((static_cast<u32>(wd) & 0x7fffu) << 1) >> 5) & 0x3ffu));
  __gx->bpSent = 0;
}

void GXSetTexCopyDst(u16 wd, u16 ht, GXTexFmt fmt, GXBool mipmap) {
  sTexCopyWidth = wd;
  sTexCopyHeight = ht;
  sTexCopyFmt = fmt;
  GX_WRITE_AURORA(GX_AURORA_LOAD_COPY_DST);
  GX_WRITE_U32(wd);
  GX_WRITE_U32(ht);
  GX_WRITE_U32(fmt);
  GX_WRITE_U8(mipmap != GX_FALSE);
}

void GXSetDispCopyFrame2Field(u32 mode) {
  SET_REG_FIELD(0, __gx->cpDisp, 2, 12, mode);
  SET_REG_FIELD(0, __gx->cpTex, 2, 12, 0);
}

void GXSetCopyClamp(GXFBClamp clamp) {
	SET_REG_FIELD(0, __gx->cpTex, 1, 0, (clamp & GX_CLAMP_TOP) != 0);
	SET_REG_FIELD(0, __gx->cpTex, 1, 1, (clamp & GX_CLAMP_BOTTOM) != 0);
}

u32 GXSetDispCopyYScale(f32 vscale) {
  const u32 iScale = y_scale_to_integer(vscale);
  GX_WRITE_RAS_REG(0x4E000000u | iScale);
  __gx->bpSent = 0;
  return num_xfb_lines(sDispCopySrcHeight, iScale);
}

void GXSetCopyClear(GXColor color, u32 depth) {
  // BP 0x4F: clear color R + A
  u32 reg0 = 0;
  SET_REG_FIELD(0, reg0, 8, 0, color.r);
  SET_REG_FIELD(0, reg0, 8, 8, color.a);
  SET_REG_FIELD(0, reg0, 8, 24, 0x4F);
  GX_WRITE_RAS_REG(reg0);

  // BP 0x50: clear color B + G
  u32 reg1 = 0;
  SET_REG_FIELD(0, reg1, 8, 0, color.b);
  SET_REG_FIELD(0, reg1, 8, 8, color.g);
  SET_REG_FIELD(0, reg1, 8, 24, 0x50);
  GX_WRITE_RAS_REG(reg1);

  // BP 0x51: clear Z (24-bit)
  u32 reg2 = 0;
  SET_REG_FIELD(0, reg2, 24, 0, depth);
  SET_REG_FIELD(0, reg2, 8, 24, 0x51);
  GX_WRITE_RAS_REG(reg2);
  __gx->bpSent = 1;
}

void GXSetCopyFilter(GXBool aa, u8 sample_pattern[12][2], GXBool vf, u8 vfilter[7]) {
  u8 samples[12][2];
  for (int i = 0; i < 12; ++i) {
    for (int j = 0; j < 2; ++j) {
      samples[i][j] = aa && sample_pattern != nullptr ? sample_pattern[i][j] : 6;
    }
  }
  for (u32 reg = 0; reg < 4; ++reg) {
    u32 value = (0x01u + reg) << 24;
    for (u32 i = 0; i < 6; ++i) {
      const u32 sample = reg * 6 + i;
      value |= (static_cast<u32>(samples[sample / 2][sample % 2]) & 0x0fu) << (i * 4);
    }
    GX_WRITE_RAS_REG(value);
  }

  static constexpr u8 kNoFilter[7] = {0, 0, 21, 22, 21, 0, 0};
  const u8* taps = vf && vfilter != nullptr ? vfilter : kNoFilter;
  GX_WRITE_RAS_REG(0x53000000u | (static_cast<u32>(taps[0] & 0x3fu) << 0) | (static_cast<u32>(taps[1] & 0x3fu) << 6) |
                   (static_cast<u32>(taps[2] & 0x3fu) << 12) | (static_cast<u32>(taps[3] & 0x3fu) << 18));
  GX_WRITE_RAS_REG(0x54000000u | (static_cast<u32>(taps[4] & 0x3fu) << 0) | (static_cast<u32>(taps[5] & 0x3fu) << 6) |
                   (static_cast<u32>(taps[6] & 0x3fu) << 12));
  __gx->bpSent = 0;
}

void GXSetDispCopyGamma(GXGamma gamma) { SET_REG_FIELD(0, __gx->cpDisp, 2, 7, gamma); }

void GXCopyDisp(void* dest, GXBool clear) {
  (void)dest;
  if (__gx->dirtyState != 0) {
    __GXSetDirtyState();
  }
  u32 control = __gx->cpDisp;
  SET_REG_FIELD(0, control, 1, 11, clear != GX_FALSE);
  SET_REG_FIELD(0, control, 1, 14, 1);
  SET_REG_FIELD(0, control, 8, 24, 0x52);
  GX_WRITE_RAS_REG(control);
  __gx->bpSent = 0;
}

void GXCopyTex(void* dest, GXBool clear) {
  GX_WRITE_AURORA(GX_AURORA_LOAD_COPY_DEST);
  GX_WRITE_U64(reinterpret_cast<u64>(dest));

  SET_REG_FIELD(0, __gx->cpTex, 1, 11, clear != GX_FALSE);
  SET_REG_FIELD(0, __gx->cpTex, 1, 14, 0);
  SET_REG_FIELD(0, __gx->cpTex, 8, 24, 0x52);
  GX_WRITE_RAS_REG(__gx->cpTex);
  read_back_probe(dest);
  aurora::gx::fifo::publish();
}

static u32 getXfbLines(u32 height, u32 scale) {
	if (height == 0 || scale == 0) {
		return 0;
	}

	u32 lines = (height - 1) * 256 / scale + 1;
	if (scale > 128 && scale < 256) {
		while ((scale & 1) == 0) {
			scale >>= 1;
		}

		if (height % scale == 0) {
			++lines;
		}
	}

	if (lines > 1024) {
		lines = 1024;
	}

	return lines;
}

u16 GXGetNumXfbLines(u16 efbHeight, f32 yScale) {
	if (!(yScale >= 1.0f && yScale <= 256.0f)) {
		return 0;
	}

	return static_cast<u16>(getXfbLines(efbHeight, static_cast<u32>(256.0f / yScale) & 511));
}

f32 GXGetYScaleFactor(u16 efbHeight, u16 xfbHeight) {
	if (efbHeight == 0 || xfbHeight < efbHeight || xfbHeight > 1024) {
		return 1.0f;
	}

	u32 target = xfbHeight;
	f32 scale = static_cast<f32>(target) / efbHeight;
	u32 lines = getXfbLines(efbHeight, static_cast<u32>(256.0f / scale) & 511);
	while (lines > xfbHeight && target > 1) {
		--target;
		scale = static_cast<f32>(target) / efbHeight;
		lines = getXfbLines(efbHeight, static_cast<u32>(256.0f / scale) & 511);
	}

	f32 result = scale;
	while (lines < xfbHeight && target < static_cast<u32>(efbHeight) * 256) {
		result = scale;
		++target;
		scale = static_cast<f32>(target) / efbHeight;
		lines = getXfbLines(efbHeight, static_cast<u32>(256.0f / scale) & 511);
	}

	return result;
}
namespace {
// The bounding box is never computed from draws, so reads return the last cleared box.
u16 sBoundingBox[4] = {1023, 0, 1023, 0};
} // namespace

void GXClearBoundingBox(void) {
  GX_WRITE_RAS_REG(0x550003FFu);
  GX_WRITE_RAS_REG(0x560003FFu);
  __gx->bpSent = 0;
  sBoundingBox[0] = 1023;
  sBoundingBox[1] = 0;
  sBoundingBox[2] = 1023;
  sBoundingBox[3] = 0;
}

void GXReadBoundingBox(u16* left, u16* right, u16* top, u16* bottom) {
  if (left != nullptr) {
    *left = sBoundingBox[0];
  }
  if (right != nullptr) {
    *right = sBoundingBox[1];
  }
  if (top != nullptr) {
    *top = sBoundingBox[2];
  }
  if (bottom != nullptr) {
    *bottom = sBoundingBox[3];
  }
}
}
