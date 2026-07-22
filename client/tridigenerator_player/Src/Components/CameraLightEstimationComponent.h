#pragma once

#include "ColorMatchingSettings.h"

struct CameraLightEstimationComponent : ColorMatchingSettings {
    bool diagnosticOverlay = false;
    float updateRateHz = 10.0f;
    float maximumFrameAgeSeconds = 0.25f;
    float estimateHoldSeconds = 1.0f;
};
