#pragma once

#include <algorithm>
#include <cmath>

namespace ScaleControl {
constexpr float LogarithmicStep = 0.025f;

inline float Clamp(float scale, float minimumScale, float maximumScale) {
    return std::clamp(scale, minimumScale, maximumScale);
}

inline float ToLogarithmicPosition(float scale) {
    return std::log10(std::max(scale, 0.000001f));
}

inline float FromLogarithmicPosition(float position) {
    return std::pow(10.0f, position);
}

inline float StepLogarithmically(
        float currentScale, int direction, bool locked,
        float minimumScale, float maximumScale) {
    if (locked || direction == 0) return Clamp(currentScale, minimumScale, maximumScale);
    const float nextPosition = ToLogarithmicPosition(currentScale) +
        static_cast<float>(direction) * LogarithmicStep;
    return Clamp(FromLogarithmicPosition(nextPosition), minimumScale, maximumScale);
}

inline float ResolveGestureScale(
        float rawScale, float currentScale, bool locked,
        float minimumScale, float maximumScale) {
    return locked ? Clamp(currentScale, minimumScale, maximumScale)
                  : Clamp(rawScale, minimumScale, maximumScale);
}

inline bool NeedsRebaseline(bool lockedAtBaseline, bool lockedNow) {
    return lockedAtBaseline != lockedNow;
}
} // namespace ScaleControl
