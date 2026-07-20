#pragma once

struct CameraLightEstimationComponent {
    bool enabled = true;
    bool diagnosticOverlay = false;
    float matchingStrength = 1.0f;
    float updateRateHz = 10.0f;
    float temporalSmoothing = 0.85f;
    float minExposure = 0.35f;
    float maxExposure = 2.0f;
    float minTint = 0.7f;
    float maxTint = 1.4f;
    float maximumFrameAgeSeconds = 0.25f;
    float estimateHoldSeconds = 1.0f;
};
