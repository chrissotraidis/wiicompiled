#include "gx/kartpad_vertex_repack.hpp"

#include <gtest/gtest.h>

#include <bit>
#include <cstring>

using namespace aurora::gx;

namespace {
AttrConfig attr(GXAttrType type, u8 cnt, u8 comp, u8 offset, u8 stride = 0, u8 frac = 0, u8 nrmIndexCount = 1) {
  return AttrConfig{.attrType = static_cast<u8>(type), .cnt = cnt, .compType = comp, .offset = offset,
                    .stride = stride, .frac = frac, .le = false, .nrmIndexCount = nrmIndexCount};
}

float be_float(const std::vector<u8>& out, size_t at) {
  const u32 bits = (u32(out[at]) << 24) | (u32(out[at + 1]) << 16) | (u32(out[at + 2]) << 8) | u32(out[at + 3]);
  return std::bit_cast<float>(bits);
}

void put_be_float(u8* p, float value) {
  const u32 bits = std::bit_cast<u32>(value);
  p[0] = u8(bits >> 24), p[1] = u8(bits >> 16), p[2] = u8(bits >> 8), p[3] = u8(bits);
}

AttrArray array(const void* data, u32 size, u8 stride, bool le = false) {
  return AttrArray{.data = data, .size = size, .stride = stride, .le = le, .cachedRange = {}};
}
} // namespace

// Layout of the failing #193 character recipe 58866e32bada1f83: 7-byte vertices with a direct
// matrix index, two direct texture-matrix indices and four 8-bit array indices.
TEST(KartPadVertexRepack, CharacterRecipeBecomesAlignedDirectData) {
  std::array<AttrConfig, MaxVtxAttr> src{};
  src[GX_VA_PNMTXIDX] = attr(GX_DIRECT, 1, GX_U8, 0);
  src[GX_VA_TEX1MTXIDX] = attr(GX_DIRECT, 1, GX_U8, 1);
  src[GX_VA_TEX2MTXIDX] = attr(GX_DIRECT, 1, GX_U8, 2);
  src[GX_VA_POS] = attr(GX_INDEX8, 3, GX_F32, 3, 12);
  src[GX_VA_NRM] = attr(GX_INDEX8, 3, GX_S16, 4, 6, 14);
  src[GX_VA_CLR0] = attr(GX_INDEX8, 1, GX_RGBA8, 5, 4);
  src[GX_VA_TEX0] = attr(GX_INDEX8, 2, GX_S16, 6, 4, 14);

  auto dst = src;
  const u8 stride = kartpad_repack::repacked_layout(dst);
  EXPECT_EQ(stride, 48);
  for (u32 i = GX_VA_PNMTXIDX; i <= GX_VA_TEX7; ++i) {
    if (dst[i].attrType == GX_NONE) continue;
    EXPECT_EQ(dst[i].attrType, GX_DIRECT) << i;
    EXPECT_EQ(dst[i].offset % 4, 0) << i;
  }
  EXPECT_EQ(dst[GX_VA_POS].offset, 12);
  EXPECT_EQ(dst[GX_VA_NRM].offset, 24);
  EXPECT_EQ(dst[GX_VA_CLR0].offset, 36);
  EXPECT_EQ(dst[GX_VA_TEX0].offset, 40);
  EXPECT_EQ(dst[GX_VA_POS].compType, GX_F32);
  EXPECT_EQ(dst[GX_VA_NRM].compType, GX_F32);
  EXPECT_EQ(dst[GX_VA_NRM].frac, 0);
  EXPECT_EQ(dst[GX_VA_CLR0].compType, GX_RGBA8);

  u8 positions[24]{};
  put_be_float(positions + 12, 1.5f);
  put_be_float(positions + 16, -2.25f);
  put_be_float(positions + 20, 1024.0f);
  const u8 normals[12] = {0, 0, 0, 0, 0, 0, 0x40, 0x00, 0xC0, 0x00, 0x20, 0x00}; // 1.0, -1.0, 0.5 at frac 14
  const u8 colors[8] = {1, 2, 3, 4, 0x10, 0x20, 0x30, 0x40};
  const u8 texcoords[8] = {0, 0, 0, 0, 0xF0, 0x00, 0x30, 0x00}; // -0.25, 0.75 at frac 14
  std::array<AttrArray, MaxVtxAttr> arrays{};
  arrays[GX_VA_POS] = array(positions, sizeof(positions), 12);
  arrays[GX_VA_NRM] = array(normals, sizeof(normals), 6);
  arrays[GX_VA_CLR0] = array(colors, sizeof(colors), 4);
  arrays[GX_VA_TEX0] = array(texcoords, sizeof(texcoords), 4);

  // Two vertices: matrix slot 10 (raw 30) and slot 0; the second uses element 1 of every array.
  const u8 vertices[14] = {30, 3, 6, 0, 0, 0, 0, 0, 9, 12, 1, 1, 1, 1};
  std::vector<u8> out;
  kartpad_repack::repack_with_layout(src, 7, dst, stride, vertices, 2, arrays, out);
  ASSERT_EQ(out.size(), 96u);

  const u8 first[12] = {30, 0, 0, 0, 3, 0, 0, 0, 6, 0, 0, 0};
  EXPECT_EQ(std::memcmp(out.data(), first, sizeof(first)), 0);
  EXPECT_EQ(be_float(out, 12), 0.0f);
  EXPECT_EQ(out[36], 1);
  EXPECT_EQ(out[39], 4);

  const size_t v = 48;
  EXPECT_EQ(out[v + 0], 0);
  EXPECT_EQ(out[v + 4], 9);
  EXPECT_EQ(out[v + 8], 12);
  EXPECT_EQ(be_float(out, v + 12), 1.5f);
  EXPECT_EQ(be_float(out, v + 16), -2.25f);
  EXPECT_EQ(be_float(out, v + 20), 1024.0f);
  EXPECT_EQ(be_float(out, v + 24), 1.0f);
  EXPECT_EQ(be_float(out, v + 28), -1.0f);
  EXPECT_EQ(be_float(out, v + 32), 0.5f);
  EXPECT_EQ(out[v + 36], 0x10);
  EXPECT_EQ(out[v + 37], 0x20);
  EXPECT_EQ(out[v + 38], 0x30);
  EXPECT_EQ(out[v + 39], 0x40);
  EXPECT_EQ(be_float(out, v + 40), -0.25f);
  EXPECT_EQ(be_float(out, v + 44), 0.75f);
}

TEST(KartPadVertexRepack, Index16Nbt3LittleEndianColorsAndOutOfRange) {
  std::array<AttrConfig, MaxVtxAttr> src{};
  src[GX_VA_PNMTXIDX] = attr(GX_DIRECT, 1, GX_U8, 0);
  src[GX_VA_POS] = attr(GX_INDEX16, 3, GX_S16, 1, 6, 8);
  src[GX_VA_NRM] = attr(GX_INDEX8, 9, GX_S8, 3, 3, 6, 3);
  src[GX_VA_CLR0] = attr(GX_INDEX8, 1, GX_RGB565, 6, 2);
  src[GX_VA_TEX0] = attr(GX_DIRECT, 2, GX_U8, 7, 0, 1);
  auto dst = src;
  const u8 stride = kartpad_repack::repacked_layout(dst);
  // pnmtx 4 + pos 12 + nbt 36 + rgb565 padded 4 + tex 8.
  EXPECT_EQ(stride, 64);

  const u8 positions[12] = {0x01, 0x80, 0xFF, 0x00, 0x00, 0x40, 0, 0, 0, 0, 0, 0}; // 1.5, -1.0, 0.25 at frac 8
  const u8 normals[9] = {64, 0, 0, 0, 0xC0, 0, 0, 0, 32}; // x=1, y=-1, z=0.5 at frac 6
  const u8 colors[2] = {0x34, 0x12};                     // little-endian 0x1234
  std::array<AttrArray, MaxVtxAttr> arrays{};
  arrays[GX_VA_POS] = array(positions, sizeof(positions), 6);
  arrays[GX_VA_NRM] = array(normals, sizeof(normals), 3);
  arrays[GX_VA_CLR0] = array(colors, sizeof(colors), 2, true);

  // Vertex 0: valid. Vertex 1: position index 0x0100 lies outside the array and reads as zero.
  const u8 vertices[18] = {27, 0x00, 0x00, 0, 1, 2, 0, 5, 3,
                           27, 0x01, 0x00, 0, 1, 2, 0, 5, 3};
  std::vector<u8> out;
  kartpad_repack::repack_with_layout(src, 9, dst, stride, vertices, 2, arrays, out);
  ASSERT_EQ(out.size(), 128u);
  EXPECT_EQ(out[0], 27);
  EXPECT_EQ(be_float(out, 4), 1.5f);
  EXPECT_EQ(be_float(out, 8), -1.0f);
  EXPECT_EQ(be_float(out, 12), 0.25f);
  EXPECT_EQ(be_float(out, 16), 1.0f);  // normal
  EXPECT_EQ(be_float(out, 32), -1.0f); // tangent y
  EXPECT_EQ(be_float(out, 48), 0.5f);  // binormal z
  EXPECT_EQ(out[52], 0x12);            // RGB565 is now big endian
  EXPECT_EQ(out[53], 0x34);
  EXPECT_EQ(be_float(out, 56), 2.5f);  // 5 at frac 1
  EXPECT_EQ(be_float(out, 60), 1.5f);  // 3 at frac 1
  EXPECT_EQ(be_float(out, 64 + 4), 0.0f);
  EXPECT_EQ(be_float(out, 64 + 16), 1.0f);
}
