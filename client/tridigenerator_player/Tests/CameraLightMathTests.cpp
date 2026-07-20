#include <cassert>
#include <cmath>
#include <vector>

#include "Systems/CameraLightMath.h"

int main() {
    using namespace CameraLightMath;

    const Rgb black = YuvToLinear(16.0f / 255.0f, 0.5f, 0.5f, false);
    assert(black.r < 0.001f && black.g < 0.001f && black.b < 0.001f);

    const Rgb gray = YuvToLinear(0.5f, 0.5f, 0.5f, true);
    assert(std::abs(gray.r - gray.g) < 0.0001f);
    assert(std::abs(gray.g - gray.b) < 0.0001f);

    assert(VoxelIndex(0, 0, 0) == 0);
    assert(VoxelIndex(15, 11, 15) == 16 * 12 * 16 - 1);
    assert(ClampGain(0.1f, 0.35f, 2.0f) == 0.35f);
    assert(ClampGain(3.0f, 0.35f, 2.0f) == 2.0f);

    assert(std::abs(TrimmedMean({0.0f, 1.0f, 2.0f, 3.0f, 100.0f}, 0.2f) - 2.0f) < 0.0001f);
    assert(IsFrameFresh(900000000, 1000000000, 0.25f));
    assert(!IsFrameFresh(500000000, 1000000000, 0.25f));
    assert(!IsFrameFresh(1100000000, 1000000000, 0.25f));
    return 0;
}
