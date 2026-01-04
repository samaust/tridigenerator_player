#pragma once

#include <openxr/openxr.h>

#include <vector>

#include "OVR_Math.h"

#include "Render/GlTexture.h"

struct EnvironmentDepthState {
    static constexpr int kNumEyes = 2;

    bool IsInitialized = false;
    bool HasDepth = false;

    uint32_t SwapchainLength = 0;
    uint32_t Width = 0;
    uint32_t Height = 0;

    std::vector<OVRFW::GlTexture> SwapchainTextures;

    XrEnvironmentDepthImageAcquireInfoMETA AcquireInfo{
        XR_TYPE_ENVIRONMENT_DEPTH_IMAGE_ACQUIRE_INFO_META};
    XrEnvironmentDepthImageMETA Image{XR_TYPE_ENVIRONMENT_DEPTH_IMAGE_META};

    float NearZ = 0.0f;
    float FarZ = 0.0f;
    OVR::Matrix4f DepthViewMatrices[kNumEyes];
    OVR::Matrix4f DepthProjectionMatrices[kNumEyes];
};
