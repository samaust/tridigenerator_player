#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace DepthTextureResize {

inline uint32_t SourceCoordinate(
        uint32_t destinationCoordinate,
        uint32_t sourceSize,
        uint32_t destinationSize) {
    if (sourceSize <= 1 || destinationSize <= 1) return 0;
    const uint64_t numerator =
        static_cast<uint64_t>(destinationCoordinate) * (sourceSize - 1) +
        (destinationSize - 1) / 2;
    return static_cast<uint32_t>(numerator / (destinationSize - 1));
}

inline bool Nearest(
        const std::vector<uint16_t>& source,
        uint32_t sourceWidth,
        uint32_t sourceHeight,
        uint32_t destinationWidth,
        uint32_t destinationHeight,
        std::vector<uint16_t>& destination) {
    const size_t sourcePixels =
        static_cast<size_t>(sourceWidth) * sourceHeight;
    if (sourceWidth == 0 || sourceHeight == 0 ||
        destinationWidth == 0 || destinationHeight == 0 ||
        source.size() < sourcePixels) {
        destination.clear();
        return false;
    }

    destination.resize(
        static_cast<size_t>(destinationWidth) * destinationHeight);
    for (uint32_t y = 0; y < destinationHeight; ++y) {
        const uint32_t sourceY =
            SourceCoordinate(y, sourceHeight, destinationHeight);
        for (uint32_t x = 0; x < destinationWidth; ++x) {
            const uint32_t sourceX =
                SourceCoordinate(x, sourceWidth, destinationWidth);
            destination[static_cast<size_t>(y) * destinationWidth + x] =
                source[static_cast<size_t>(sourceY) * sourceWidth + sourceX];
        }
    }
    return true;
}

} // namespace DepthTextureResize
