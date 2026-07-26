#pragma once

#include <array>

#include <openxr/openxr.h>

#include "Input/ControllerRenderer.h"
#include "Input/HandRenderer.h"

struct InputState {
    std::array<XrHandTrackerEXT, 2> handTrackers{XR_NULL_HANDLE, XR_NULL_HANDLE};
    std::array<OVRFW::ControllerRenderer, 2> controllerRenderers{};
    std::array<OVRFW::HandRenderer, 2> handRenderers{};
    std::array<bool, 2> controllerRendererInitialized{false, false};
    std::array<bool, 2> handRendererInitialized{false, false};
    std::array<bool, 2> previousPinch{false, false};
    std::array<bool, 2> previousGrip{false, false};
    std::array<OVR::Posef, 2> previousControllerPose{};
    std::array<bool, 2> previousControllerPoseValid{false, false};
    std::array<double, 2> lastControllerActivitySeconds{-1.0, -1.0};
    double lastHandTrackingActiveSeconds{-1.0};
    bool clapArmed{true};
    bool sessionInitialized{false};
};
