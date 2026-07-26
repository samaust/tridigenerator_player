#include "Components/MeshDetailSettings.h"
#include "Systems/DepthFramePreparation.h"

#include <cassert>
#include <cstdint>
#include <vector>

int main() {
    assert(MeshDetailSettings{}.divisor == 2);
    for (int divisor = 1; divisor <= 4; ++divisor) {
        assert(MeshDetailControl::IsValidDivisor(divisor));
        assert(MeshDetailControl::SanitizeDivisor(divisor) == divisor);
    }
    assert(!MeshDetailControl::IsValidDivisor(0));
    assert(!MeshDetailControl::IsValidDivisor(5));
    assert(MeshDetailControl::SanitizeDivisor(0) == 2);
    assert(MeshDetailControl::SanitizeDivisor(99) == 2);

    assert(MeshDetailControl::ReducedDimension(1920, 1) == 1920);
    assert(MeshDetailControl::ReducedDimension(1920, 2) == 960);
    assert(MeshDetailControl::ReducedDimension(1920, 3) == 640);
    assert(MeshDetailControl::ReducedDimension(1920, 4) == 480);
    assert(MeshDetailControl::ReducedDimension(1081, 2) == 541);
    assert(MeshDetailControl::ReducedDimension(1081, 3) == 361);
    assert(MeshDetailControl::ReducedDimension(1, 4) == 2);
    assert(MeshDetailControl::ReducedDimension(0, 4) == 2);

    assert(MeshDetailControl::VertexCount(1920, 1080, 1) == 2073600);
    assert(MeshDetailControl::VertexCount(1920, 1080, 2) == 518400);
    assert(MeshDetailControl::VertexCount(1920, 1080, 3) == 230400);
    assert(MeshDetailControl::VertexCount(1920, 1080, 4) == 129600);

    assert(DepthFramePreparer::SourceCoordinate(0, 5, 3) == 0);
    assert(DepthFramePreparer::SourceCoordinate(1, 5, 3) == 2);
    assert(DepthFramePreparer::SourceCoordinate(2, 5, 3) == 4);
    const std::vector<uint16_t> source{
        0, 1, 2, 3, 4,
        5, 6, 7, 8, 9,
        10, 11, 12, 13, 14};
    DepthFramePreparer preparer;
    PreparedDepthFrame prepared;
    assert(preparer.Prepare(
        source, 5, 3, 3, 2, 65535, 1.0f, {1.0f, 1.0f, 0.0f, 0.0f},
        prepared));
    assert((prepared.pixels ==
        std::vector<uint16_t>{0, 2, 4, 10, 12, 14}));
    assert(prepared.boundsValid);
    assert((prepared.boundsMinimum == std::array<float, 3>{0.0f, -28.0f, -14.0f}));
    assert((prepared.boundsMaximum == std::array<float, 3>{56.0f, 0.0f, 0.0f}));
    assert(preparer.Prepare(
        source, 5, 3, 5, 3, 65535, 1.0f, {1.0f, 1.0f, 0.0f, 0.0f},
        prepared));
    assert(prepared.pixels == source);
    assert(prepared.boundsValid);
    assert(!preparer.Prepare(
        source, 0, 3, 3, 2, 65535, 1.0f, {1.0f, 1.0f, 0.0f, 0.0f},
        prepared));
    assert(prepared.pixels.empty());

    // Strided and deliberately unaligned raw planes must produce exactly the
    // same samples and bounds for either schema-v4 byte order.
    constexpr uint32_t rawWidth = 7;
    constexpr uint32_t rawHeight = 5;
    constexpr int rawStride = 17;
    std::vector<uint16_t> rawValues(rawWidth * rawHeight);
    for (size_t i = 0; i < rawValues.size(); ++i) {
        rawValues[i] = i == 9 ? 65535 : static_cast<uint16_t>(100 + i * 3);
    }
    std::vector<uint8_t> bigEndianStorage(
        1 + static_cast<size_t>(rawStride) * rawHeight, 0xcc);
    std::vector<uint8_t> littleEndianStorage(
        1 + static_cast<size_t>(rawStride) * rawHeight, 0xdd);
    for (uint32_t y = 0; y < rawHeight; ++y) {
        for (uint32_t x = 0; x < rawWidth; ++x) {
            const uint16_t value = rawValues[
                static_cast<size_t>(y) * rawWidth + x];
            uint8_t* be = bigEndianStorage.data() + 1 +
                static_cast<size_t>(y) * rawStride + x * 2;
            uint8_t* le = littleEndianStorage.data() + 1 +
                static_cast<size_t>(y) * rawStride + x * 2;
            be[0] = static_cast<uint8_t>(value >> 8);
            be[1] = static_cast<uint8_t>(value);
            le[0] = static_cast<uint8_t>(value);
            le[1] = static_cast<uint8_t>(value >> 8);
        }
    }
    for (int divisor = 1; divisor <= 4; ++divisor) {
        const uint32_t destinationWidth =
            MeshDetailControl::ReducedDimension(rawWidth, divisor);
        const uint32_t destinationHeight =
            MeshDetailControl::ReducedDimension(rawHeight, divisor);
        PreparedDepthFrame reference;
        PreparedDepthFrame fromBigEndian;
        PreparedDepthFrame fromLittleEndian;
        assert(preparer.Prepare(
            rawValues, rawWidth, rawHeight,
            destinationWidth, destinationHeight, 65535, 1000.0f,
            {4.0f, 4.0f, 3.0f, 2.0f}, reference));
        assert(preparer.Prepare(
            bigEndianStorage.data() + 1, rawWidth, rawHeight, rawStride, true,
            destinationWidth, destinationHeight, 65535, 1000.0f,
            {4.0f, 4.0f, 3.0f, 2.0f}, fromBigEndian));
        assert(preparer.Prepare(
            littleEndianStorage.data() + 1, rawWidth, rawHeight, rawStride, false,
            destinationWidth, destinationHeight, 65535, 1000.0f,
            {4.0f, 4.0f, 3.0f, 2.0f}, fromLittleEndian));
        assert(fromBigEndian.pixels == reference.pixels);
        assert(fromLittleEndian.pixels == reference.pixels);
        assert(fromBigEndian.boundsValid == reference.boundsValid);
        assert(fromLittleEndian.boundsValid == reference.boundsValid);
        assert(fromBigEndian.boundsMinimum == reference.boundsMinimum);
        assert(fromBigEndian.boundsMaximum == reference.boundsMaximum);
        assert(fromLittleEndian.boundsMinimum == reference.boundsMinimum);
        assert(fromLittleEndian.boundsMaximum == reference.boundsMaximum);
    }
    assert(!preparer.Prepare(
        bigEndianStorage.data() + 1, rawWidth, rawHeight,
        static_cast<int>(rawWidth * 2 - 1), true,
        3, 2, 65535, 1000.0f, {4.0f, 4.0f, 3.0f, 2.0f}, prepared));
    return 0;
}
