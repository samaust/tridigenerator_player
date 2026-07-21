#pragma once

struct TransformState {
    OVR::Matrix4f animationMatrix = OVR::Matrix4f::Identity();
    OVR::Matrix4f modelMatrix = OVR::Matrix4f::Identity();
};
