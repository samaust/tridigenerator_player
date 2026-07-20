#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace CameraLightMath {

struct Rgb { float r, g, b; };

inline Rgb YuvToLinear(float y, float u, float v, bool fullRange) {
    const float d = u - 0.5f;
    const float e = v - 0.5f;
    Rgb c{};
    if (fullRange) {
        c = {y + 1.402f * e, y - 0.344136f * d - 0.714136f * e, y + 1.772f * d};
    } else {
        const float yy = y - 0.0625f;
        c = {1.1643f * yy + 1.5958f * e,
             1.1643f * yy - 0.39173f * d - 0.81290f * e,
             1.1643f * yy + 2.017f * d};
    }
    auto linear = [](float value) {
        value = std::clamp(value, 0.0f, 1.0f);
        return value <= 0.04045f ? value / 12.92f :
                std::pow((value + 0.055f) / 1.055f, 2.4f);
    };
    return {linear(c.r), linear(c.g), linear(c.b)};
}

inline int VoxelIndex(int x, int y, int z, int width = 16, int height = 12) {
    return x + width * (y + height * z);
}

inline float ClampGain(float value, float minimum, float maximum) {
    return std::clamp(value, minimum, maximum);
}

inline float TrimmedMean(std::vector<float> values, float trimFraction = 0.1f) {
    if (values.empty()) return 0.0f;
    std::sort(values.begin(), values.end());
    const size_t trim = std::min(values.size() / 2, static_cast<size_t>(values.size() * trimFraction));
    float sum = 0.0f;
    for (size_t i = trim; i < values.size() - trim; ++i) sum += values[i];
    return sum / static_cast<float>(values.size() - trim * 2);
}

inline bool IsFrameFresh(int64_t sensorTimestampNs, int64_t nowNs, float maximumAgeSeconds) {
    return sensorTimestampNs > 0 && nowNs >= sensorTimestampNs &&
            (nowNs - sensorTimestampNs) <= static_cast<int64_t>(maximumAgeSeconds * 1.0e9f);
}

} // namespace CameraLightMath
