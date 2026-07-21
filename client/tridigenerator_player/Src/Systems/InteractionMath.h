#pragma once

#include <algorithm>
#include <cmath>

#include "OVR_Math.h"

namespace InteractionMath {

inline bool RayAabb(
        const OVR::Vector3f& origin,
        const OVR::Vector3f& direction,
        const OVR::Vector3f& minimum,
        const OVR::Vector3f& maximum,
        float& distance) {
    float nearDistance = 0.0f;
    float farDistance = 1.0e6f;
    for (int axis = 0; axis < 3; ++axis) {
        const float o = origin[axis];
        const float d = direction[axis];
        if (std::fabs(d) < 1.0e-6f) {
            if (o < minimum[axis] || o > maximum[axis]) return false;
            continue;
        }
        float first = (minimum[axis] - o) / d;
        float second = (maximum[axis] - o) / d;
        if (first > second) std::swap(first, second);
        nearDistance = std::max(nearDistance, first);
        farDistance = std::min(farDistance, second);
        if (nearDistance > farDistance) return false;
    }
    distance = nearDistance;
    return farDistance >= 0.0f;
}

inline OVR::Quatf AlignVectors(OVR::Vector3f from, OVR::Vector3f to) {
    if (from.LengthSq() < 1.0e-8f || to.LengthSq() < 1.0e-8f) return OVR::Quatf::Identity();
    from.Normalize();
    to.Normalize();
    return OVR::Quatf::Align(to, from);
}

inline float ApplyDeadZone(float value, float deadZone) {
    if (std::fabs(value) <= deadZone) return 0.0f;
    return std::copysign((std::fabs(value) - deadZone) / (1.0f - deadZone), value);
}

} // namespace InteractionMath
