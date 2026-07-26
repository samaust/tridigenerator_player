#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

struct PreparedDepthFrame {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint16_t> pixels;
    bool boundsValid = false;
    std::array<float, 3> boundsMinimum{};
    std::array<float, 3> boundsMaximum{};
};

class DepthFramePreparer {
public:
    bool Prepare(
            const std::vector<uint16_t>& source,
            uint32_t sourceWidth,
            uint32_t sourceHeight,
            uint32_t destinationWidth,
            uint32_t destinationHeight,
            uint16_t invalidDepthValue,
            float depthUnitsPerMetre,
            const std::array<float, 4>& intrinsics,
            PreparedDepthFrame& output) {
        const size_t sourcePixels =
            static_cast<size_t>(sourceWidth) * sourceHeight;
        if (sourceWidth == 0 || sourceHeight == 0 ||
            destinationWidth == 0 || destinationHeight == 0 ||
            source.size() < sourcePixels ||
            depthUnitsPerMetre <= 0.0f ||
            intrinsics[0] == 0.0f || intrinsics[1] == 0.0f) {
            output = {};
            return false;
        }

        RefreshMapping(
            sourceWidth,
            sourceHeight,
            destinationWidth,
            destinationHeight,
            intrinsics);
        output.width = destinationWidth;
        output.height = destinationHeight;
        const bool resizePixels =
            sourceWidth != destinationWidth ||
            sourceHeight != destinationHeight;
        if (resizePixels) {
            output.pixels.resize(
                static_cast<size_t>(destinationWidth) * destinationHeight);
        } else {
            output.pixels.clear();
        }
        output.boundsValid = false;

        std::array<float, 3> minimum{
            std::numeric_limits<float>::max(),
            std::numeric_limits<float>::max(),
            std::numeric_limits<float>::max()};
        std::array<float, 3> maximum{
            std::numeric_limits<float>::lowest(),
            std::numeric_limits<float>::lowest(),
            std::numeric_limits<float>::lowest()};

        for (uint32_t y = 0; y < destinationHeight; ++y) {
            const size_t sourceRow =
                static_cast<size_t>(sourceY_[y]) * sourceWidth;
            const size_t destinationRow =
                static_cast<size_t>(y) * destinationWidth;
            for (uint32_t x = 0; x < destinationWidth; ++x) {
                const uint16_t encoded = source[sourceRow + sourceX_[x]];
                if (resizePixels) {
                    output.pixels[destinationRow + x] = encoded;
                }
                if (encoded == invalidDepthValue) continue;

                const float z =
                    static_cast<float>(encoded) / depthUnitsPerMetre;
                const std::array<float, 3> point{
                    rayX_[x] * z,
                    rayY_[y] * z,
                    -z};
                for (size_t axis = 0; axis < point.size(); ++axis) {
                    minimum[axis] = std::min(minimum[axis], point[axis]);
                    maximum[axis] = std::max(maximum[axis], point[axis]);
                }
                output.boundsValid = true;
            }
        }

        if (output.boundsValid) {
            output.boundsMinimum = minimum;
            output.boundsMaximum = maximum;
        } else {
            output.boundsMinimum = {};
            output.boundsMaximum = {};
        }
        return true;
    }

    static uint32_t SourceCoordinate(
            uint32_t destinationCoordinate,
            uint32_t sourceSize,
            uint32_t destinationSize) {
        if (sourceSize <= 1 || destinationSize <= 1) return 0;
        const uint64_t numerator =
            static_cast<uint64_t>(destinationCoordinate) * (sourceSize - 1) +
            (destinationSize - 1) / 2;
        return static_cast<uint32_t>(numerator / (destinationSize - 1));
    }

private:
    void RefreshMapping(
            uint32_t sourceWidth,
            uint32_t sourceHeight,
            uint32_t destinationWidth,
            uint32_t destinationHeight,
            const std::array<float, 4>& intrinsics) {
        if (sourceWidth_ == sourceWidth &&
            sourceHeight_ == sourceHeight &&
            destinationWidth_ == destinationWidth &&
            destinationHeight_ == destinationHeight &&
            intrinsics_ == intrinsics) {
            return;
        }

        sourceWidth_ = sourceWidth;
        sourceHeight_ = sourceHeight;
        destinationWidth_ = destinationWidth;
        destinationHeight_ = destinationHeight;
        intrinsics_ = intrinsics;
        sourceX_.resize(destinationWidth);
        rayX_.resize(destinationWidth);
        for (uint32_t x = 0; x < destinationWidth; ++x) {
            sourceX_[x] =
                SourceCoordinate(x, sourceWidth, destinationWidth);
            rayX_[x] =
                (static_cast<float>(sourceX_[x]) - intrinsics[2]) /
                intrinsics[0];
        }
        sourceY_.resize(destinationHeight);
        rayY_.resize(destinationHeight);
        for (uint32_t y = 0; y < destinationHeight; ++y) {
            sourceY_[y] =
                SourceCoordinate(y, sourceHeight, destinationHeight);
            rayY_[y] =
                -(static_cast<float>(sourceY_[y]) - intrinsics[3]) /
                intrinsics[1];
        }
    }

    uint32_t sourceWidth_ = 0;
    uint32_t sourceHeight_ = 0;
    uint32_t destinationWidth_ = 0;
    uint32_t destinationHeight_ = 0;
    std::array<float, 4> intrinsics_{};
    std::vector<uint32_t> sourceX_;
    std::vector<uint32_t> sourceY_;
    std::vector<float> rayX_;
    std::vector<float> rayY_;
};
