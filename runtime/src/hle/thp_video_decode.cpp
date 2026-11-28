#include "hle_stubs.h"

#include <cstdint>
#include <vector>
#include "memory.h"
#include "recomp_mod_loader.h"
#include "thp_decoder.h"

extern "C" void GxNotifyGuestRamDmaWrite(uint32_t addr, uint32_t size);

// Native THPVideoDecode. The translated SDK decoder takes most of the video decode thread's time
// in the menus (THP::VideoDecoder, #584), more than a slow phone has left over each frame.
// work (the SDK's scratch THPFileInfo) is unused: nothing outside the decoder reads it.

namespace {

// A frame's video data is well under this (about 85 KB for the largest movie on the disc).
constexpr size_t kMaxFrameBytes = 1u << 20;

bool HostAccessible(uint32_t address, size_t length) {
    return !GuestFlat::RequiresCheckedAccess() && Memory::Contains(address, length) &&
           !MemoryInline::RangeHasDeferredReads(address, length);
}

// The input has no size argument; the decoder stops at the end of the scan, so allow the
// largest span below kMaxFrameBytes that stays inside the guest region.
size_t ReadableBytes(uint32_t address) {
    size_t length = kMaxFrameBytes;
    while (length > 0 && !Memory::Contains(address, length)) {
        length >>= 1;
    }
    return length;
}

void CopyToGuest(uint32_t address, const std::vector<uint8_t>& bytes) {
    for (size_t i = 0; i < bytes.size(); ++i) {
        Memory::Write8(address + static_cast<uint32_t>(i), bytes[i]);
    }
}

} // namespace

extern "C" uint32_t THPVideoDecode_801b3bac(uint32_t file, uint32_t tileY, uint32_t tileU, uint32_t tileV,
                                            uint32_t /*work*/)
{
    const size_t inputBytes = file == 0 ? 0 : ReadableBytes(file);
    std::vector<uint8_t> inputCopy;
    const uint8_t* input = nullptr;
    if (inputBytes != 0 && HostAccessible(file, inputBytes)) {
        input = Memory::GetPointer(file, inputBytes);
    } else if (inputBytes != 0) {
        inputCopy.resize(inputBytes);
        for (size_t i = 0; i < inputBytes; ++i) {
            inputCopy[i] = Memory::Read8(file + static_cast<uint32_t>(i));
        }
        input = inputCopy.data();
    }
    if (tileY == 0 || tileU == 0 || tileV == 0) {
        return static_cast<uint32_t>(ThpDecoder::DecodeFrame(input, inputBytes, nullptr, nullptr, nullptr));
    }

    ThpDecoder::FrameSize frameSize;
    const int32_t headerResult = ThpDecoder::ReadFrameSize(input, inputBytes, frameSize);
    if (headerResult != 0) {
        return static_cast<uint32_t>(headerResult);
    }
    const size_t lumaBytes = ThpDecoder::TiledPlaneBytes(frameSize.width, frameSize.height);
    const size_t chromaBytes = ThpDecoder::TiledPlaneBytes((frameSize.width + 1) / 2, (frameSize.height + 1) / 2);

    int32_t result = 0;
    if (HostAccessible(tileY, lumaBytes) && HostAccessible(tileU, chromaBytes) &&
        HostAccessible(tileV, chromaBytes) && !RecompMod::ExecutableWriteGuardMayHit(tileY, lumaBytes) &&
        !RecompMod::ExecutableWriteGuardMayHit(tileU, chromaBytes) &&
        !RecompMod::ExecutableWriteGuardMayHit(tileV, chromaBytes)) {
        result = ThpDecoder::DecodeFrame(input, inputBytes, Memory::GetPointer(tileY, lumaBytes),
                                         Memory::GetPointer(tileU, chromaBytes),
                                         Memory::GetPointer(tileV, chromaBytes));
    } else {
        std::vector<uint8_t> luma(lumaBytes);
        std::vector<uint8_t> chromaU(chromaBytes);
        std::vector<uint8_t> chromaV(chromaBytes);
        result = ThpDecoder::DecodeFrame(input, inputBytes, luma.data(), chromaU.data(), chromaV.data());
        CopyToGuest(tileY, luma);
        CopyToGuest(tileU, chromaU);
        CopyToGuest(tileV, chromaV);
    }

    // The SDK decoder writes the planes by locked-cache DMA, which tells GX the textures changed.
    GxNotifyGuestRamDmaWrite(tileY, static_cast<uint32_t>(lumaBytes));
    GxNotifyGuestRamDmaWrite(tileU, static_cast<uint32_t>(chromaBytes));
    GxNotifyGuestRamDmaWrite(tileV, static_cast<uint32_t>(chromaBytes));
    return static_cast<uint32_t>(result);
}

PPC_NATIVE_OVERRIDE(801B3BAC, THPVideoDecode_801b3bac, uint32_t,
                    (uint32_t file, uint32_t tileY, uint32_t tileU, uint32_t tileV, uint32_t work),
                    (file, tileY, tileU, tileV, work));
