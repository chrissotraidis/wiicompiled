// Adapted from aurora's lib/dolphin/thp/THPDec.cpp (https://github.com/encounter/aurora, MIT,
// Copyright (c) 2022 Luke Street), which reproduces the SDK decoder's float AAN inverse DCT.
// Changes: no aurora dependencies, a 64-bit bit buffer, a lookup table for short Huffman codes
// and whole-row tile writes, so a frame costs a fraction of the translated SDK decoder.
#include "thp_decoder.h"

#include <array>
#include <cmath>
#include <cstring>

namespace ThpDecoder {
namespace {

// The SDK's THPVideoDecode error codes.
constexpr int32_t kBadSyntax = 3;
constexpr int32_t kBadPrecision = 10;
constexpr int32_t kUnsupportedMarker = 11;
constexpr int32_t kBadComponentCount = 12;
constexpr int32_t kMissingHuffmanTable = 15;
constexpr int32_t kBadSampling = 19;
constexpr int32_t kNoInput = 25;
constexpr int32_t kNoOutput = 27;

constexpr std::array<uint8_t, 64> kNaturalOrder{0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,
                                                12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6,  7,  14, 21, 28,
                                                35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
                                                58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};

constexpr std::array<double, 8> kAanScale{1.0f, 1.387039845f, 1.306562965f, 1.175875602f,
                                          1.0f, 0.785694958f, 0.541196100f, 0.275899379f};

// AAN butterfly constants
constexpr float kSqrt2 = 1.414213562f;
constexpr float kC2 = 1.847759065f;        // 2 cos(pi/8)
constexpr float kC2MinusC6 = 1.082392200f; // 2 cos(pi/8) - 2 cos(3pi/8)
constexpr float kC2PlusC6 = 2.613125930f;  // 2 cos(pi/8) + 2 cos(3pi/8)
constexpr float kC6 = kC2 - kC2MinusC6;

constexpr float kOutputBias = 128.0f * 8.0f;

// Huffman codes up to this many bits are decoded with one table lookup.
constexpr int kLookupBits = 9;

struct QuantizationTable {
    std::array<float, 64> values{};
    bool valid = false;
};

struct HuffmanTable {
    std::array<uint8_t, 17> counts{};
    std::array<uint16_t, 17> firstCodes{};
    std::array<uint16_t, 17> symbolOffsets{};
    std::array<uint8_t, 256> symbols{};
    // For each kLookupBits-bit prefix: code length in the high byte, symbol in the low byte.
    // Zero means the code is longer than kLookupBits.
    std::array<uint16_t, 1 << kLookupBits> lookup{};
    bool valid = false;
};

struct Component {
    uint8_t quantizationTable = 0;
    uint8_t dcTable = 0;
    uint8_t acTable = 0;
    int32_t predictedDc = 0;
};

struct DecodeContext {
    std::array<QuantizationTable, 3> quantizationTables{};
    std::array<HuffmanTable, 4> huffmanTables{};
    std::array<Component, 3> components{};
    uint16_t width = 0;
    uint16_t height = 0;
    uint16_t restartInterval = 0;
    size_t scanOffset = 0;
};

uint16_t ReadBigEndian16(const uint8_t* data) {
    return static_cast<uint16_t>((data[0] << 8) | data[1]);
}

int32_t ParseQuantizationTables(const uint8_t* data, size_t size, DecodeContext& context) {
    size_t position = 0;
    while (position < size) {
        if (size - position < 65) {
            return kBadSyntax;
        }
        const uint8_t descriptor = data[position++];
        const uint8_t precision = descriptor >> 4;
        const uint8_t id = descriptor & 15;
        if (precision != 0 || id >= context.quantizationTables.size()) {
            return kBadSyntax;
        }

        std::array<float, 64> natural{};
        for (size_t i = 0; i < 64; ++i) {
            natural[kNaturalOrder[i]] = static_cast<float>(data[position++]);
        }
        QuantizationTable& table = context.quantizationTables[id];
        for (size_t row = 0; row < 8; ++row) {
            for (size_t column = 0; column < 8; ++column) {
                const size_t index = row * 8 + column;
                table.values[index] =
                    static_cast<float>(static_cast<double>(natural[index]) * kAanScale[row] * kAanScale[column]);
            }
        }
        table.valid = true;
    }
    return 0;
}

int32_t ParseFrameHeader(const uint8_t* data, size_t size, DecodeContext& context) {
    if (size < 6) {
        return kBadSyntax;
    }
    if (data[0] != 8) {
        return kBadPrecision;
    }
    context.height = ReadBigEndian16(data + 1);
    context.width = ReadBigEndian16(data + 3);
    if (data[5] != 3) {
        return kBadComponentCount;
    }
    if (size < 15 || context.width == 0 || context.height == 0) {
        return kBadSyntax;
    }

    for (size_t i = 0; i < 3; ++i) {
        const uint8_t sampling = data[7 + i * 3];
        if ((i == 0 && sampling != 0x22) || (i != 0 && sampling != 0x11)) {
            return kBadSampling;
        }
        const uint8_t table = data[8 + i * 3];
        if (table >= context.quantizationTables.size()) {
            return kBadSyntax;
        }
        context.components[i].quantizationTable = table;
    }
    return 0;
}

int32_t ParseHuffmanTables(const uint8_t* data, size_t size, DecodeContext& context) {
    size_t position = 0;
    while (position < size) {
        if (size - position < 17) {
            return kBadSyntax;
        }
        const uint8_t descriptor = data[position++];
        const uint8_t tableClass = descriptor >> 4;
        const uint8_t id = descriptor & 15;
        if (tableClass > 1 || id > 1) {
            return kBadSyntax;
        }

        HuffmanTable table{};
        size_t symbolCount = 0;
        for (size_t length = 1; length <= 16; ++length) {
            table.counts[length] = data[position++];
            symbolCount += table.counts[length];
        }
        if (symbolCount > table.symbols.size() || symbolCount > size - position) {
            return kBadSyntax;
        }
        std::memcpy(table.symbols.data(), data + position, symbolCount);
        position += symbolCount;

        uint32_t code = 0;
        uint16_t symbolOffset = 0;
        for (int length = 1; length <= 16; ++length) {
            const uint32_t count = table.counts[length];
            if (code + count > (uint32_t{1} << length)) {
                return kBadSyntax;
            }
            table.firstCodes[length] = static_cast<uint16_t>(code);
            table.symbolOffsets[length] = symbolOffset;
            if (length <= kLookupBits) {
                for (uint32_t i = 0; i < count; ++i) {
                    const uint32_t first = (code + i) << (kLookupBits - length);
                    const uint16_t entry =
                        static_cast<uint16_t>((length << 8) | table.symbols[symbolOffset + i]);
                    for (uint32_t fill = 0; fill < (uint32_t{1} << (kLookupBits - length)); ++fill) {
                        table.lookup[first + fill] = entry;
                    }
                }
            }
            code = (code + count) << 1;
            symbolOffset = static_cast<uint16_t>(symbolOffset + count);
        }
        table.valid = true;
        context.huffmanTables[id * 2 + tableClass] = table;
    }
    return 0;
}

int32_t ParseScanHeader(const uint8_t* data, size_t size, DecodeContext& context) {
    if (size < 1) {
        return kBadSyntax;
    }
    if (data[0] != 3) {
        return kBadComponentCount;
    }
    if (size < 10) {
        return kBadSyntax;
    }

    for (size_t i = 0; i < 3; ++i) {
        const uint8_t selectors = data[2 + i * 2];
        const uint8_t dcTable = selectors >> 4;
        const uint8_t acTable = selectors & 15;
        if (dcTable > 1 || acTable > 1 || !context.huffmanTables[dcTable * 2].valid ||
            !context.huffmanTables[acTable * 2 + 1].valid) {
            return kMissingHuffmanTable;
        }
        context.components[i].dcTable = dcTable;
        context.components[i].acTable = acTable;
        context.components[i].predictedDc = 0;
    }
    if (data[7] != 0 || data[8] != 63 || data[9] != 0) {
        return kBadSyntax;
    }
    return 0;
}

// Reads markers up to the start of scan; on success context.scanOffset is the first scan byte.
int32_t ParseHeaders(const uint8_t* data, size_t size, DecodeContext& context) {
    size_t position = 0;
    while (true) {
        if (position >= size || data[position++] != 0xFF) {
            return kBadSyntax;
        }
        uint8_t marker = 0xFF;
        while (marker == 0xFF) {
            if (position >= size) {
                return kBadSyntax;
            }
            marker = data[position++];
        }
        if (marker == 0xD8) {
            continue;
        }

        if (size - position < 2) {
            return kBadSyntax;
        }
        const uint16_t segmentSize = ReadBigEndian16(data + position);
        if (segmentSize < 2 || segmentSize > size - position) {
            return kBadSyntax;
        }
        const uint8_t* segment = data + position + 2;
        const size_t payloadSize = segmentSize - 2u;
        position += segmentSize;

        int32_t result = 0;
        switch (marker) {
        case 0xC0:
            result = ParseFrameHeader(segment, payloadSize, context);
            break;
        case 0xC4:
            result = ParseHuffmanTables(segment, payloadSize, context);
            break;
        case 0xDA:
            result = ParseScanHeader(segment, payloadSize, context);
            context.scanOffset = position;
            return result;
        case 0xDB:
            result = ParseQuantizationTables(segment, payloadSize, context);
            break;
        case 0xDD:
            if (payloadSize != 2) {
                return kBadSyntax;
            }
            context.restartInterval = ReadBigEndian16(segment);
            break;
        default:
            if ((marker < 0xE0 || marker > 0xEF) && marker != 0xFE) {
                return kUnsupportedMarker;
            }
            break;
        }
        if (result != 0) {
            return result;
        }
    }
}

// MSB-first reader over the entropy-coded data. Reading past the end yields zero bits; Valid()
// reports whether anything past the end was consumed. Refill() leaves at least 57 bits buffered,
// enough for one Huffman code (16 bits) and its value bits (16), so a coefficient needs one refill.
class BitReader {
public:
    BitReader(const uint8_t* data, size_t size, size_t byteOffset)
        : mData{data}, mSize{size}, mPosition{byteOffset} {}

    void Refill() {
        if (mSize - mPosition >= 8 && mPosition <= mSize) {
            // Load eight bytes at once; the bits past the whole bytes taken are loaded again
            // (with the same values) by the next refill.
            uint64_t next = 0;
            for (int i = 0; i < 8; ++i) {
                next = (next << 8) | mData[mPosition + i];
            }
            mBuffer |= next >> mBitCount;
            const int bytesTaken = (63 - mBitCount) >> 3;
            mPosition += bytesTaken;
            mBitCount += bytesTaken * 8;
            return;
        }
        RefillNearEnd();
    }

    uint32_t Peek(int count) const { return static_cast<uint32_t>(mBuffer >> (64 - count)); }

    void Skip(int count) {
        mBuffer <<= count;
        mBitCount -= count;
    }

    uint32_t Read(int count) {
        if (count == 0) {
            return 0;
        }
        const uint32_t value = Peek(count);
        Skip(count);
        return value;
    }

    void ByteAlign() { Skip(mBitCount & 7); }

    bool Valid() const { return mPosition * 8 - static_cast<size_t>(mBitCount) <= mSize * 8; }

private:
    void RefillNearEnd() {
        while (mBitCount <= 56) {
            const uint64_t byte = mPosition < mSize ? mData[mPosition] : 0;
            ++mPosition;
            mBuffer |= byte << (56 - mBitCount);
            mBitCount += 8;
        }
    }

    const uint8_t* mData;
    size_t mSize;
    size_t mPosition;
    uint64_t mBuffer = 0;
    int mBitCount = 0;
};

bool DecodeLongHuffman(BitReader& reader, const HuffmanTable& table, uint8_t& symbol) {
    const uint32_t bits = reader.Peek(16);
    for (int length = kLookupBits + 1; length <= 16; ++length) {
        const uint32_t code = bits >> (16 - length);
        const uint32_t firstCode = table.firstCodes[length];
        if (code >= firstCode && code - firstCode < table.counts[length]) {
            reader.Skip(length);
            symbol = table.symbols[table.symbolOffsets[length] + code - firstCode];
            return true;
        }
    }
    return false;
}

bool DecodeHuffman(BitReader& reader, const HuffmanTable& table, uint8_t& symbol) {
    const uint16_t entry = table.lookup[reader.Peek(kLookupBits)];
    if (entry == 0) {
        return DecodeLongHuffman(reader, table, symbol);
    }
    reader.Skip(entry >> 8);
    symbol = static_cast<uint8_t>(entry);
    return true;
}

int32_t ExtendValue(uint32_t value, uint8_t bitCount) {
    if (bitCount != 0 && value < (uint32_t{1} << (bitCount - 1))) {
        return static_cast<int32_t>(value) - static_cast<int32_t>(uint32_t{1} << bitCount) + 1;
    }
    return static_cast<int32_t>(value);
}

bool DecodeBlock(BitReader& reader, DecodeContext& context, size_t componentIndex, std::array<int16_t, 64>& block) {
    block.fill(0);
    Component& component = context.components[componentIndex];
    const HuffmanTable& dcTable = context.huffmanTables[component.dcTable * 2];
    const HuffmanTable& acTable = context.huffmanTables[component.acTable * 2 + 1];

    uint8_t bitCount = 0;
    reader.Refill();
    if (!DecodeHuffman(reader, dcTable, bitCount) || bitCount > 16) {
        return false;
    }
    component.predictedDc += ExtendValue(reader.Read(bitCount), bitCount);
    block[0] = static_cast<int16_t>(component.predictedDc);

    size_t coefficient = 1;
    while (coefficient < 64) {
        reader.Refill();
        uint8_t runAndSize = 0;
        if (!DecodeHuffman(reader, acTable, runAndSize)) {
            return false;
        }
        const uint8_t run = runAndSize >> 4;
        bitCount = runAndSize & 15;
        if (bitCount == 0) {
            if (run == 15) {
                coefficient += 16;
                continue;
            }
            break;
        }

        coefficient += run;
        if (coefficient >= 64) {
            return false;
        }
        block[kNaturalOrder[coefficient]] = static_cast<int16_t>(ExtendValue(reader.Read(bitCount), bitCount));
        ++coefficient;
    }
    return true;
}

using FloatRow = std::array<float, 8>;

struct EvenHalf {
    float out0, out1, out2, out3;
};

struct OddHalf {
    float out7, out6, out5, out4;
};

EvenHalf AanEven(float sum04, float dif04, float sum26, float dif26) {
    const float rotated = std::fma(dif26, kSqrt2, -sum26);
    return {sum04 + sum26, dif04 + rotated, dif04 - rotated, sum04 - sum26};
}

OddHalf AanOdd(float z10, float z11, float z12, float z13) {
    const float out7 = z11 + z13;
    const float z5 = (z10 + z12) * kC2;
    const float out6 = std::fma(-z10, kC2PlusC6, z5) - out7;
    const float out5 = std::fma(z11 - z13, kSqrt2, -out6);
    const float out4 = std::fma(-z12, kC2MinusC6, z5) - out5;
    return {out7, out6, out5, out4};
}

FloatRow Combine(const EvenHalf& even, const OddHalf& odd) {
    return {
        even.out0 + odd.out7, even.out1 + odd.out6, even.out2 + odd.out5, even.out3 + odd.out4,
        even.out3 - odd.out4, even.out2 - odd.out5, even.out1 - odd.out6, even.out0 - odd.out7,
    };
}

FloatRow TransformRow(const int16_t* coefficients, const float* quant) {
    const float x0 = static_cast<float>(coefficients[0]) * quant[0];
    const float x1 = static_cast<float>(coefficients[1]) * quant[1];
    const float x2 = static_cast<float>(coefficients[2]) * quant[2];
    const float x3 = static_cast<float>(coefficients[3]) * quant[3];
    const auto c4 = static_cast<float>(coefficients[4]);
    const auto c5 = static_cast<float>(coefficients[5]);
    const auto c6 = static_cast<float>(coefficients[6]);
    const auto c7 = static_cast<float>(coefficients[7]);
    const EvenHalf even = AanEven(std::fma(c4, quant[4], x0), std::fma(-c4, quant[4], x0),
                                  std::fma(c6, quant[6], x2), std::fma(-c6, quant[6], x2));
    const OddHalf odd = AanOdd(std::fma(c5, quant[5], -x3), std::fma(c7, quant[7], x1),
                               std::fma(-c7, quant[7], x1), std::fma(c5, quant[5], x3));
    return Combine(even, odd);
}

FloatRow TransformRowLow4(const FloatRow& x) {
    const float sum02 = x[0] + x[2];
    const float dif02 = x[0] - x[2];
    const EvenHalf even{sum02, std::fma(x[2], kSqrt2, dif02), std::fma(-x[2], kSqrt2, sum02), dif02};
    const OddHalf odd = AanOdd(-x[3], x[1], x[1], x[3]);
    return Combine(even, odd);
}

FloatRow TransformRowDcAc(float dc, float ac) {
    const float a1 = std::fma(ac, kC2, -ac);
    const float a2 = std::fma(ac, kSqrt2, -a1);
    const float a3 = std::fma(-ac, kC6, a2);
    return {dc + ac, dc + a1, dc + a2, dc - a3, dc + a3, dc - a2, dc - a1, dc - ac};
}

FloatRow TransformColumn(const FloatRow& x) {
    const EvenHalf even =
        AanEven((x[0] + x[4]) + kOutputBias, (x[0] - x[4]) + kOutputBias, x[2] + x[6], x[2] - x[6]);
    const OddHalf odd = AanOdd(x[5] - x[3], x[1] + x[7], x[1] - x[7], x[5] + x[3]);
    return Combine(even, odd);
}

size_t RowExtent(const int16_t* coefficients) {
    size_t extent = 8;
    while (extent > 0 && coefficients[extent - 1] == 0) {
        --extent;
    }
    return extent;
}

std::array<uint8_t, 64> InverseDct(const std::array<int16_t, 64>& coefficients, const QuantizationTable& quantization) {
    std::array<float, 64> workspace{};
    for (size_t row = 0; row < 8; ++row) {
        const int16_t* rowCoefficients = &coefficients[row * 8];
        const float* rowQuant = &quantization.values[row * 8];
        FloatRow transformed{};
        switch (RowExtent(rowCoefficients)) {
        case 0:
        case 1:
            transformed.fill(static_cast<float>(rowCoefficients[0]) * rowQuant[0]);
            break;
        case 2:
            transformed = TransformRowDcAc(static_cast<float>(rowCoefficients[0]) * rowQuant[0],
                                           static_cast<float>(rowCoefficients[1]) * rowQuant[1]);
            break;
        case 3:
        case 4: {
            FloatRow dequantized{};
            for (size_t column = 0; column < 4; ++column) {
                dequantized[column] = static_cast<float>(rowCoefficients[column]) * rowQuant[column];
            }
            transformed = TransformRowLow4(dequantized);
            break;
        }
        default:
            transformed = TransformRow(rowCoefficients, rowQuant);
            break;
        }
        std::memcpy(&workspace[row * 8], transformed.data(), sizeof(transformed));
    }

    // Column pass and conversion as separate loops over contiguous data, which the compiler
    // turns into vector code.
    std::array<float, 64> columns{};
    for (size_t column = 0; column < 8; ++column) {
        FloatRow input{};
        for (size_t row = 0; row < 8; ++row) {
            input[row] = workspace[row * 8 + column];
        }
        const FloatRow transformed = TransformColumn(input);
        for (size_t row = 0; row < 8; ++row) {
            columns[row * 8 + column] = transformed[row];
        }
    }
    std::array<uint8_t, 64> output{};
    for (size_t i = 0; i < 64; ++i) {
        // Clamp to 0..255, then truncate (the SDK's conversion), without branches.
        output[i] = static_cast<uint8_t>(std::fmin(std::fmax(columns[i] * 0.125f, 0.0f), 255.0f));
    }
    return output;
}

// Blocks start on multiples of 8, so each block row is 8 consecutive bytes of one tile row.
void WriteBlock(uint8_t* output, uint16_t width, uint16_t height, uint16_t blockX, uint16_t blockY,
                const std::array<uint8_t, 64>& pixels) {
    const size_t tilesPerRow = (width + 7) / 8;
    const bool fullWidth = width - blockX >= 8;
    for (uint16_t row = 0; row < 8 && blockY + row < height; ++row) {
        const uint32_t y = blockY + row;
        uint8_t* tileRow = output + (static_cast<size_t>(y / 4) * tilesPerRow + blockX / 8) * 32 + (y & 3) * 8;
        if (fullWidth) {
            std::memcpy(tileRow, &pixels[row * 8], 8);
        } else {
            std::memcpy(tileRow, &pixels[row * 8], width - blockX);
        }
    }
}

bool DecodeAndWriteBlock(BitReader& reader, DecodeContext& context, size_t component, uint8_t* output,
                         uint16_t width, uint16_t height, uint16_t x, uint16_t y) {
    std::array<int16_t, 64> coefficients{};
    if (!DecodeBlock(reader, context, component, coefficients)) {
        return false;
    }
    const QuantizationTable& quantization =
        context.quantizationTables[context.components[component].quantizationTable];
    if (!quantization.valid) {
        return false;
    }
    WriteBlock(output, width, height, x, y, InverseDct(coefficients, quantization));
    return true;
}

} // namespace

int32_t ReadFrameSize(const uint8_t* data, size_t size, FrameSize& frameSize) {
    if (data == nullptr) {
        return kNoInput;
    }
    DecodeContext context{};
    const int32_t result = ParseHeaders(data, size, context);
    frameSize = {context.width, context.height};
    return result;
}

int32_t DecodeFrame(const uint8_t* data, size_t size, uint8_t* tileY, uint8_t* tileU, uint8_t* tileV) {
    if (data == nullptr) {
        return kNoInput;
    }
    if (tileY == nullptr || tileU == nullptr || tileV == nullptr) {
        return kNoOutput;
    }

    DecodeContext context{};
    const int32_t headerResult = ParseHeaders(data, size, context);
    if (headerResult != 0) {
        return headerResult;
    }

    const uint16_t chromaWidth = static_cast<uint16_t>((context.width + 1) / 2);
    const uint16_t chromaHeight = static_cast<uint16_t>((context.height + 1) / 2);

    BitReader bits{data, size, context.scanOffset};
    const uint16_t mcuColumns = static_cast<uint16_t>((context.width + 15) / 16);
    const uint16_t mcuRows = static_cast<uint16_t>((context.height + 15) / 16);
    uint32_t restartCount = 0;
    for (uint16_t mcuY = 0; mcuY < mcuRows; ++mcuY) {
        for (uint16_t mcuX = 0; mcuX < mcuColumns; ++mcuX) {
            const uint16_t lumaX = static_cast<uint16_t>(mcuX * 16);
            const uint16_t lumaY = static_cast<uint16_t>(mcuY * 16);
            const bool decoded =
                DecodeAndWriteBlock(bits, context, 0, tileY, context.width, context.height, lumaX, lumaY) &&
                DecodeAndWriteBlock(bits, context, 0, tileY, context.width, context.height,
                                    static_cast<uint16_t>(lumaX + 8), lumaY) &&
                DecodeAndWriteBlock(bits, context, 0, tileY, context.width, context.height, lumaX,
                                    static_cast<uint16_t>(lumaY + 8)) &&
                DecodeAndWriteBlock(bits, context, 0, tileY, context.width, context.height,
                                    static_cast<uint16_t>(lumaX + 8), static_cast<uint16_t>(lumaY + 8)) &&
                DecodeAndWriteBlock(bits, context, 1, tileU, chromaWidth, chromaHeight,
                                    static_cast<uint16_t>(mcuX * 8), static_cast<uint16_t>(mcuY * 8)) &&
                DecodeAndWriteBlock(bits, context, 2, tileV, chromaWidth, chromaHeight,
                                    static_cast<uint16_t>(mcuX * 8), static_cast<uint16_t>(mcuY * 8));
            if (!decoded) {
                return kBadSyntax;
            }

            if (context.restartInterval != 0 && ++restartCount == context.restartInterval) {
                bits.ByteAlign();
                restartCount = 0;
                for (Component& component : context.components) {
                    component.predictedDc = 0;
                }
            }
        }
    }
    return bits.Valid() ? 0 : kBadSyntax;
}

} // namespace ThpDecoder
