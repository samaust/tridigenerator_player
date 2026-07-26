#pragma once

#include "ColorMatchingSettings.h"

struct CameraLightEstimationComponent : ColorMatchingSettings {
    bool diagnosticOverlay = false;
    // Estimation cadence is intentionally lower than the sensor cadence so the
    // selected image is fresh without making camera work part of every XR frame.
    float updateRateHz = 5.0f;
    float maximumFrameAgeSeconds = 0.25f;
    float estimateHoldSeconds = 1.0f;
};
