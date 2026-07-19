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
    bool sessionInitialized{false};
};
