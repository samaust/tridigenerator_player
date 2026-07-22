#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include <openxr/openxr.h>

#include "OVR_Math.h"

struct ControllerInput {
    bool tracked{false};
    OVR::Posef gripPose{};
    OVR::Posef aimPose{};
    OVR::Vector2f joystick{0.0f, 0.0f};
    float indexTrigger{0.0f};
    float gripTrigger{0.0f};
    bool indexClick{false};
    bool gripPressed{false};
    bool gripPressedThisFrame{false};
    bool gripReleasedThisFrame{false};
};

struct HandInput {
    bool active{false};
    bool aimValid{false};
    bool indexPinching{false};
    bool pinchPressedThisFrame{false};
    bool pinchReleasedThisFrame{false};
    OVR::Posef aimPose{};
    OVR::Posef palmPose{};
    std::array<XrHandJointLocationEXT, XR_HAND_JOINT_COUNT_EXT> joints{};
};

struct InputComponent {
    static constexpr size_t Left = 0;
    static constexpr size_t Right = 1;

    std::array<ControllerInput, 2> controllers{};
    std::array<HandInput, 2> hands{};
    uint32_t buttons{0};
    uint32_t lastButtons{0};
    uint32_t touches{0};
    uint32_t lastTouches{0};
    bool leftXPressedThisFrame{false};
    bool rightAPressedThisFrame{false};
    bool uiToggleRequested{false};
};
