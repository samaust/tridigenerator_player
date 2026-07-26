#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

struct DynamicCellCullingData {
    std::uint32_t meshWidth = 0;
    std::uint32_t meshHeight = 0;
    std::vector<std::uint8_t> retainedCells;
    std::uint32_t retainedCellCount = 0;
    std::uint32_t rejectedCellCount = 0;
    std::uint32_t mixedCellCount = 0;
    std::uint64_t visibilityVersion = 0;

    bool Matches(
            std::uint32_t width,
            std::uint32_t height,
            std::uint64_t version) const {
        return width >= 2 && height >= 2 &&
            meshWidth == width && meshHeight == height &&
            visibilityVersion == version &&
            retainedCells.size() ==
                static_cast<std::size_t>(width - 1) * (height - 1);
    }
};
