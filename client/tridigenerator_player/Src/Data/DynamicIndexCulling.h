#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "Render/DynamicCellCullingData.h"
#include "MaskVisibilitySnapshot.h"

namespace DynamicIndexCulling {

inline std::uint32_t RectangleSum(
        const std::vector<std::uint32_t>& integral,
        std::uint32_t stride,
        std::uint32_t x0,
        std::uint32_t y0,
        std::uint32_t x1,
        std::uint32_t y1) {
    const std::uint32_t right = x1 + 1;
    const std::uint32_t bottom = y1 + 1;
    return integral[static_cast<std::size_t>(bottom) * stride + right] -
        integral[static_cast<std::size_t>(y0) * stride + right] -
        integral[static_cast<std::size_t>(bottom) * stride + x0] +
        integral[static_cast<std::size_t>(y0) * stride + x0];
}

inline std::uint32_t FirstTexel(
        std::uint32_t cell,
        std::uint32_t meshSize,
        std::uint32_t textureSize) {
    if (textureSize <= 1 || meshSize <= 1) return 0;
    const std::uint64_t numerator =
        static_cast<std::uint64_t>(cell) * textureSize;
    const std::uint32_t first = static_cast<std::uint32_t>(
        numerator / (meshSize - 1));
    return first > 0 ? first - 1 : 0;
}

inline std::uint32_t LastTexel(
        std::uint32_t cell,
        std::uint32_t meshSize,
        std::uint32_t textureSize) {
    if (textureSize <= 1 || meshSize <= 1) return 0;
    const std::uint64_t numerator =
        static_cast<std::uint64_t>(cell + 1) * textureSize +
        (meshSize - 2);
    const std::uint32_t last = static_cast<std::uint32_t>(
        numerator / (meshSize - 1));
    return std::min(textureSize - 1, last + 1);
}

inline bool Prepare(
        const std::vector<std::uint8_t>& maskIds,
        std::uint32_t maskWidth,
        std::uint32_t maskHeight,
        const std::vector<std::uint16_t>& depth,
        std::uint32_t meshWidth,
        std::uint32_t meshHeight,
        const MaskVisibilitySnapshot& visibility,
        DynamicCellCullingData& output) {
    const std::size_t maskPixels =
        static_cast<std::size_t>(maskWidth) * maskHeight;
    const std::size_t depthPixels =
        static_cast<std::size_t>(meshWidth) * meshHeight;
    if (maskWidth == 0 || maskHeight == 0 ||
        meshWidth < 2 || meshHeight < 2 ||
        maskIds.size() < maskPixels || depth.size() < depthPixels) {
        output = {};
        return false;
    }

    output.meshWidth = meshWidth;
    output.meshHeight = meshHeight;
    output.visibilityVersion = visibility.version;
    output.retainedCellCount = 0;
    output.rejectedCellCount = 0;
    output.mixedCellCount = 0;
    output.retainedCells.assign(
        static_cast<std::size_t>(meshWidth - 1) * (meshHeight - 1), 0);

    const std::uint32_t maskStride = maskWidth + 1;
    std::vector<std::uint32_t> visibleMaskIntegral(
        static_cast<std::size_t>(maskStride) * (maskHeight + 1), 0);
    for (std::uint32_t y = 0; y < maskHeight; ++y) {
        std::uint32_t rowSum = 0;
        for (std::uint32_t x = 0; x < maskWidth; ++x) {
            rowSum += visibility.IsVisible(
                maskIds[static_cast<std::size_t>(y) * maskWidth + x]) ? 1u : 0u;
            visibleMaskIntegral[
                static_cast<std::size_t>(y + 1) * maskStride + x + 1] =
                visibleMaskIntegral[
                    static_cast<std::size_t>(y) * maskStride + x + 1] +
                rowSum;
        }
    }

    const std::uint32_t depthStride = meshWidth + 1;
    std::vector<std::uint32_t> validDepthIntegral(
        static_cast<std::size_t>(depthStride) * (meshHeight + 1), 0);
    for (std::uint32_t y = 0; y < meshHeight; ++y) {
        std::uint32_t rowSum = 0;
        for (std::uint32_t x = 0; x < meshWidth; ++x) {
            rowSum += depth[
                static_cast<std::size_t>(y) * meshWidth + x] != 0 ? 1u : 0u;
            validDepthIntegral[
                static_cast<std::size_t>(y + 1) * depthStride + x + 1] =
                validDepthIntegral[
                    static_cast<std::size_t>(y) * depthStride + x + 1] +
                rowSum;
        }
    }

    for (std::uint32_t cellY = 0; cellY + 1 < meshHeight; ++cellY) {
        // Mesh UVs flip Y, but a rectangular footprint has the same conservative
        // coverage after reflecting its row range.
        const std::uint32_t reflectedY = meshHeight - 2 - cellY;
        const std::uint32_t maskY0 =
            FirstTexel(reflectedY, meshHeight, maskHeight);
        const std::uint32_t maskY1 =
            LastTexel(reflectedY, meshHeight, maskHeight);
        const std::uint32_t depthY0 =
            FirstTexel(reflectedY, meshHeight, meshHeight);
        const std::uint32_t depthY1 =
            LastTexel(reflectedY, meshHeight, meshHeight);

        for (std::uint32_t cellX = 0; cellX + 1 < meshWidth; ++cellX) {
            const std::uint32_t maskX0 =
                FirstTexel(cellX, meshWidth, maskWidth);
            const std::uint32_t maskX1 =
                LastTexel(cellX, meshWidth, maskWidth);
            const std::uint32_t maskSampleCount =
                (maskX1 - maskX0 + 1) * (maskY1 - maskY0 + 1);
            const std::uint32_t visibleMaskCount = RectangleSum(
                visibleMaskIntegral,
                maskStride,
                maskX0,
                maskY0,
                maskX1,
                maskY1);
            const bool anyVisibleMask = visibleMaskCount != 0;
            const bool allVisibleMasks =
                visibleMaskCount == maskSampleCount;

            const std::uint32_t depthX0 =
                FirstTexel(cellX, meshWidth, meshWidth);
            const std::uint32_t depthX1 =
                LastTexel(cellX, meshWidth, meshWidth);
            const std::uint32_t depthSampleCount =
                (depthX1 - depthX0 + 1) * (depthY1 - depthY0 + 1);
            const std::uint32_t validDepthCount = RectangleSum(
                validDepthIntegral,
                depthStride,
                depthX0,
                depthY0,
                depthX1,
                depthY1);
            const bool anyValidDepth = validDepthCount != 0;
            const bool allValidDepth =
                validDepthCount == depthSampleCount;

            const bool retained = anyVisibleMask && anyValidDepth;
            const std::size_t cellIndex =
                static_cast<std::size_t>(cellY) * (meshWidth - 1) + cellX;
            output.retainedCells[cellIndex] = retained ? 1 : 0;
            if (retained) {
                ++output.retainedCellCount;
                if (!allVisibleMasks || !allValidDepth) {
                    ++output.mixedCellCount;
                }
            } else {
                ++output.rejectedCellCount;
            }
        }
    }
    return true;
}

} // namespace DynamicIndexCulling
