#pragma once

#include "OVR_Math.h"

struct InteractableComponent {
    bool enabled{true};
    bool boundsValid{false};
    OVR::Vector3f localBoundsMin{-0.5f, -0.5f, -0.5f};
    OVR::Vector3f localBoundsMax{0.5f, 0.5f, 0.5f};
    float minimumScale{0.01f};
    float maximumScale{100.0f};
};
