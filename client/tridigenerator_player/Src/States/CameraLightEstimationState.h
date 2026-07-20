#pragma once

#include <cstdint>
#include <memory>

#include "OVR_Math.h"

struct CameraLightEstimationPlatformState;

enum class LightEstimateTier : int {
    Unavailable = 0,
    Global = 1,
    Spatial = 2,
};

struct CameraLightEstimationState {
    static constexpr int GridWidth = 16;
    static constexpr int GridHeight = 12;
    static constexpr int GridDepth = 16;

    std::shared_ptr<CameraLightEstimationPlatformState> platform;
    unsigned int lightFieldTexture = 0;
    unsigned int computeProgram = 0;
    unsigned int cameraTextures[3] = {0, 0, 0};
    int cameraTextureWidths[3] = {0, 0, 0};
    int cameraTextureHeights[3] = {0, 0, 0};

    OVR::Vector3f gridMinimum = {-2.0f, -1.5f, -2.0f};
    OVR::Vector3f gridExtent = {4.0f, 3.0f, 4.0f};
    OVR::Vector4f globalLight = {1.0f, 1.0f, 1.0f, 1.0f};
    OVR::Matrix4f localFromCamera;
    OVR::Matrix4f cameraFromLocal;
    OVR::Vector4f cameraIntrinsics = {1.0f, 1.0f, 0.0f, 0.0f};
    OVR::Vector2f cameraImageSize = {1.0f, 1.0f};

    LightEstimateTier tier = LightEstimateTier::Unavailable;
    LightEstimateTier loggedTier = LightEstimateTier::Unavailable;
    float tierBlend = 0.0f;
    double lastEstimateSeconds = 0.0;
    double lastDispatchSeconds = 0.0;
    bool cameraCalibrationValid = false;
    bool texturesReady = false;
};
