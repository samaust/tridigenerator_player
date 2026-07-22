#pragma once

#include <array>
#include <vector>

#include "OVR_Math.h"
#include "../Core/Entity.h"

enum class InteractionMode { Idle, SingleGrab, TwoHandScale };
enum class InteractionActorKind { None, Controller, Hand };
enum class HapticEvent { GrabAccepted, TwoHandStarted, GrabReleased, ScaleLimitReached, UiToggled };

struct HapticRequest {
    HapticEvent event{};
    uint8_t controllerMask{0};
};

struct InteractionActor {
    InteractionActorKind kind{InteractionActorKind::None};
    size_t side{0};
    OVR::Posef initialPose{};
};

struct InteractionState {
    InteractionMode mode{InteractionMode::Idle};
    EntityID selectedEntity{INVALID_ENTITY};
    std::array<InteractionActor, 2> actors{};
    OVR::Posef initialUserPose{};
    OVR::Vector3f initialScale{1.0f, 1.0f, 1.0f};
    float rayDistance{0.0f};
    float initialRayDistance{0.0f};
    OVR::Vector3f initialMidpoint{};
    OVR::Vector3f initialVector{};
    bool scaleLimitLatched{false};
    bool scaleLockedAtBaseline{true};
    std::vector<HapticRequest> hapticRequests{};
};
