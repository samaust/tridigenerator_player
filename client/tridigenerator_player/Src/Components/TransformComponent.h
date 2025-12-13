#pragma once

#include "OVR_Math.h"

struct TransformComponent {
    OVR::Posef modelPose = OVR::Posef::Identity();
    OVR::Vector3f modelScale = {1, 1, 1};
};
