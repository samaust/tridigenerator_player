#include <cmath>
#include <iostream>

#include "Systems/InteractionMath.h"

namespace {
int failures = 0;

void Expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}

bool Near(float first, float second, float epsilon = 1.0e-4f) {
    return std::fabs(first - second) <= epsilon;
}
} // namespace

int main() {
    float distance = -1.0f;
    Expect(InteractionMath::RayAabb(
            {0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, -1.0f},
            {-0.5f, -0.5f, -0.5f}, {0.5f, 0.5f, 0.5f}, distance),
            "forward ray intersects bounds");
    Expect(Near(distance, 0.5f), "ray reports nearest intersection");
    Expect(!InteractionMath::RayAabb(
            {2.0f, 0.0f, 1.0f}, {0.0f, 0.0f, -1.0f},
            {-0.5f, -0.5f, -0.5f}, {0.5f, 0.5f, 0.5f}, distance),
            "parallel ray outside slab misses");

    const OVR::Quatf alignment = InteractionMath::AlignVectors(
            {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f});
    const OVR::Vector3f aligned = alignment.Rotate({1.0f, 0.0f, 0.0f});
    Expect(Near(aligned.x, 0.0f) && Near(aligned.y, 1.0f),
            "alignment rotates initial vector to current vector");
    const OVR::Quatf identity = InteractionMath::AlignVectors({}, {});
    Expect(Near(identity.x, 0.0f) && Near(identity.y, 0.0f) &&
           Near(identity.z, 0.0f) && Near(identity.w, 1.0f),
           "zero vectors produce identity rotation");

    Expect(Near(InteractionMath::ApplyDeadZone(0.1f, 0.2f), 0.0f),
            "stick values inside dead zone are zero");
    Expect(Near(InteractionMath::ApplyDeadZone(1.0f, 0.2f), 1.0f),
            "dead zone preserves full positive input");
    Expect(Near(InteractionMath::ApplyDeadZone(-1.0f, 0.2f), -1.0f),
            "dead zone preserves full negative input");
    return failures == 0 ? 0 : 1;
}
