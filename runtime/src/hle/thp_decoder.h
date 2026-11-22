#pragma once

#include <cstddef>
#include <cstdint>

// Native decoder for the video frames in Nintendo THP movies (baseline JPEG, 4:2:0, no byte
// stuffing). Output matches the RVL SDK's THPVideoDecode: three I8 textures in GX tile order
// (8x4 texels per 32-byte tile), luma at full size and both chroma planes at half size.
namespace ThpDecoder {

struct FrameSize {
    uint16_t width = 0;
    uint16_t height = 0;
};

// Bytes of one I8 tiled plane of the given size.
inline size_t TiledPlaneBytes(uint32_t width, uint32_t height) {
    return static_cast<size_t>((width + 7) / 8) * ((height + 3) / 4) * 32;
}

// Reads the frame's headers. Returns 0 or one of the SDK's THPVideoDecode error codes.
int32_t ReadFrameSize(const uint8_t* data, size_t size, FrameSize& frameSize);

// Decodes one frame into the three planes. Returns 0 or an SDK error code.
int32_t DecodeFrame(const uint8_t* data, size_t size, uint8_t* tileY, uint8_t* tileU, uint8_t* tileV);

} // namespace ThpDecoder
