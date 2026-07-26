#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "OVR_Math.h"
#include "../Components/ColorMatchingControl.h"

struct CameraLightEstimationPlatformState;

enum class LightEstimateTier : int {
    Unavailable = 0,
    Global = 1,
    Spatial = 2,
};

enum class CameraPipelineMode : int {
    Unavailable = 0,
    RawExternalYuv = 1,
    CpuYuvPlanes = 2,
};

struct CameraCaptureDiagnostics {
    CameraPipelineMode pipeline = CameraPipelineMode::Unavailable;
    uint64_t callbackCount = 0;
    uint64_t processedCount = 0;
    uint64_t supersededFrameCount = 0;
    uint64_t queuePressureDrops = 0;
    uint64_t bytesCopied = 0;
    float latestFrameAgeMs = 0.0f;
    float callbackP50Ms = 0.0f;
    float callbackP95Ms = 0.0f;
    float importP50Ms = 0.0f;
    float importP95Ms = 0.0f;
};

struct CameraLightEstimationState {
    static constexpr int GridWidth = 16;
    static constexpr int GridHeight = 12;
    static constexpr int GridDepth = 16;

    std::shared_ptr<CameraLightEstimationPlatformState> platform;
    unsigned int lightFieldTexture = 0;
    unsigned int lightFieldScratchTexture = 0;
    unsigned int computeProgram = 0;
    unsigned int cameraTextures[3] = {0, 0, 0};
    int cameraTextureWidths[3] = {0, 0, 0};
    int cameraTextureHeights[3] = {0, 0, 0};

    OVR::Vector3f gridMinimum = {-2.0f, -1.5f, -2.0f};
    OVR::Vector3f gridExtent = {4.0f, 3.0f, 4.0f};
    // Captured-scene chromaticity (linear RGB, unit luminance) and luminance.
    OVR::Vector4f globalLight = {1.0f, 1.0f, 1.0f, 0.18f};
    OVR::Matrix4f localFromCamera;
    OVR::Matrix4f cameraFromLocal;
    OVR::Vector4f cameraIntrinsics = {1.0f, 1.0f, 0.0f, 0.0f};
    OVR::Vector2f cameraImageSize = {1.0f, 1.0f};

    LightEstimateTier tier = LightEstimateTier::Unavailable;
    LightEstimateTier loggedTier = LightEstimateTier::Unavailable;
    ColorMatchingTier loggedRequestedTier = ColorMatchingTier::Spatial;
    TierAvailability globalAvailability = TierAvailability::Checking;
    TierAvailability spatialAvailability = TierAvailability::Checking;
    std::string availabilityMessage;
    float tierBlend = 0.0f;
    double lastEstimateSeconds = 0.0;
    double lastCameraProcessingSeconds = 0.0;
    double lastDispatchSeconds = 0.0;
    bool cameraCalibrationValid = false;
    bool texturesReady = false;
    CameraCaptureDiagnostics captureDiagnostics;
};
