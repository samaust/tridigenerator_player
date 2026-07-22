#include "InteractionSystem.h"

#include <algorithm>
#include <cmath>

#include "InteractionMath.h"
#include "../Components/InputComponent.h"
#include "../Components/InteractableComponent.h"
#include "../Components/TransformComponent.h"
#include "../States/InputState.h"
#include "../States/InteractionState.h"
#include "../States/TransformState.h"
#include "TransformSystem.h"
#include "ScaleControl.h"

namespace {
constexpr float kStickDeadZone = 0.18f;
constexpr float kDepthSpeed = 1.25f;
constexpr float kMinimumRayDistance = 0.05f;
constexpr float kMinimumTwoHandDistance = 0.025f;
constexpr float kClapCloseDistance = 0.075f;
constexpr float kClapResetDistance = 0.16f;
constexpr float kClapClosingDelta = 0.008f;
constexpr uint8_t ControllerMask(size_t side) { return static_cast<uint8_t>(1u << side); }

OVR::Vector3f RayDirection(const OVR::Posef& aimPose) {
    return aimPose.Rotation.Rotate({0.0f, 0.0f, -1.0f}).Normalized();
}

bool PoseValid(const XrHandJointLocationEXT& joint) {
    constexpr XrSpaceLocationFlags required =
            XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
    return (joint.locationFlags & required) == required;
}

OVR::Posef ActorPose(const InputComponent& input, const InteractionActor& actor) {
    return actor.kind == InteractionActorKind::Hand
            ? input.hands[actor.side].palmPose
            : input.controllers[actor.side].gripPose;
}

OVR::Posef ActorAim(const InputComponent& input, const InteractionActor& actor) {
    return actor.kind == InteractionActorKind::Hand
            ? input.hands[actor.side].aimPose
            : input.controllers[actor.side].aimPose;
}

bool ActorHeld(const InputComponent& input, const InteractionActor& actor) {
    if (actor.kind == InteractionActorKind::Hand) {
        return input.hands[actor.side].active && input.hands[actor.side].indexPinching;
    }
    return actor.kind == InteractionActorKind::Controller &&
            input.controllers[actor.side].tracked && input.controllers[actor.side].gripPressed;
}

InteractionActor PressedActor(const InputComponent& input, size_t side) {
    const HandInput& hand = input.hands[side];
    if (hand.active && hand.aimValid && hand.pinchPressedThisFrame) {
        return {InteractionActorKind::Hand, side, hand.palmPose};
    }
    const ControllerInput& controller = input.controllers[side];
    if (controller.tracked && controller.gripPressedThisFrame) {
        return {InteractionActorKind::Controller, side, controller.gripPose};
    }
    return {};
}

void QueueHaptic(InteractionState& state, HapticEvent event, const InteractionActor& actor) {
    if (actor.kind == InteractionActorKind::Controller) {
        state.hapticRequests.push_back({event, ControllerMask(actor.side)});
    }
}

void RebaselineSingle(
        InteractionState& state,
        TransformComponent& transform,
        const InputComponent& input,
        const InteractionActor& actor) {
    state.mode = InteractionMode::SingleGrab;
    state.actors[0] = actor;
    state.actors[0].initialPose = ActorPose(input, actor);
    state.actors[1] = {};
    state.initialUserPose = transform.modelPose;
    state.initialScale = transform.modelScale;
    state.initialRayDistance = state.rayDistance;
    state.scaleLimitLatched = false;
}

bool RayHits(
        const OVR::Posef& aim,
        const TransformState& transform,
        const InteractableComponent& interactable,
        float& worldDistance) {
    const OVR::Matrix4f inverse = transform.modelMatrix.Inverted();
    const OVR::Vector3f worldOrigin = aim.Translation;
    const OVR::Vector3f worldDirection = RayDirection(aim);
    const OVR::Vector3f localOrigin = inverse.Transform(worldOrigin);
    const OVR::Vector4f localDirection4 = inverse.Transform(
            OVR::Vector4f(worldDirection.x, worldDirection.y, worldDirection.z, 0.0f));
    OVR::Vector3f localDirection(localDirection4.x, localDirection4.y, localDirection4.z);
    const float localLength = localDirection.Length();
    if (localLength < 1.0e-6f) return false;
    localDirection /= localLength;
    float localDistance = 0.0f;
    if (!InteractionMath::RayAabb(
            localOrigin, localDirection,
            interactable.localBoundsMin, interactable.localBoundsMax,
            localDistance)) return false;
    const OVR::Vector3f localHit = localOrigin + localDirection * localDistance;
    worldDistance = (transform.modelMatrix.Transform(localHit) - worldOrigin).Length();
    return true;
}
} // namespace

bool InteractionSystem::Init(EntityManager& ecs) {
    ecs.ForEach<InteractionState>([](EntityID, InteractionState& state) { state = {}; });
    return true;
}

void InteractionSystem::Shutdown(EntityManager& ecs) {
    ecs.ForEach<InteractionState>([](EntityID, InteractionState& state) { state = {}; });
}

bool InteractionSystem::IsManipulating(EntityManager& ecs) const {
    bool manipulating = false;
    ecs.ForEach<InteractionState>([&](EntityID, InteractionState& state) {
        manipulating |= state.mode != InteractionMode::Idle;
    });
    return manipulating;
}

void InteractionSystem::CancelManipulation(EntityManager& ecs) {
    ecs.ForEach<InteractionState>([](EntityID, InteractionState& state) {
        state.mode = InteractionMode::Idle;
        state.selectedEntity = INVALID_ENTITY;
        state.actors = {};
        state.scaleLimitLatched = false;
    });
}

void InteractionSystem::Update(EntityManager& ecs, float deltaSeconds) {
    InputComponent* input = nullptr;
    InputState* inputState = nullptr;
    ecs.ForEachMulti<InputComponent, InputState>([&](EntityID, InputComponent& component, InputState& state) {
        input = &component;
        inputState = &state;
    });
    if (!input || !inputState) return;

    ecs.ForEachMulti<InteractableComponent, TransformComponent, TransformState, InteractionState>(
        [&](EntityID entity, InteractableComponent& interactable, TransformComponent& transform,
            TransformState& transformState, InteractionState& state) {
        state.hapticRequests.clear();
        if (!interactable.enabled || !interactable.boundsValid) return;

        if (state.mode == InteractionMode::Idle) {
            for (size_t side = 0; side < 2; ++side) {
                InteractionActor actor = PressedActor(*input, side);
                if (actor.kind == InteractionActorKind::None) continue;
                float hitDistance = 0.0f;
                if (!RayHits(ActorAim(*input, actor), transformState, interactable, hitDistance)) continue;
                state.selectedEntity = entity;
                state.rayDistance = state.initialRayDistance = hitDistance;
                RebaselineSingle(state, transform, *input, actor);
                QueueHaptic(state, HapticEvent::GrabAccepted, actor);
                break;
            }
            return;
        }

        if (state.mode == InteractionMode::SingleGrab) {
            InteractionActor first = state.actors[0];
            if (!ActorHeld(*input, first)) {
                QueueHaptic(state, HapticEvent::GrabReleased, first);
                state.mode = InteractionMode::Idle;
                state.selectedEntity = INVALID_ENTITY;
                return;
            }

            const size_t otherSide = first.side == InputComponent::Left ? InputComponent::Right : InputComponent::Left;
            InteractionActor second = PressedActor(*input, otherSide);
            if (second.kind == first.kind && second.kind != InteractionActorKind::None) {
                float secondHit = 0.0f;
                if (RayHits(ActorAim(*input, second), transformState, interactable, secondHit)) {
                    state.mode = InteractionMode::TwoHandScale;
                    state.actors[1] = second;
                    state.actors[0].initialPose = ActorPose(*input, first);
                    state.actors[1].initialPose = ActorPose(*input, second);
                    state.initialUserPose = transform.modelPose;
                    state.initialScale = transform.modelScale;
                    state.initialMidpoint = (state.actors[0].initialPose.Translation +
                                             state.actors[1].initialPose.Translation) * 0.5f;
                    state.initialVector = state.actors[1].initialPose.Translation -
                                          state.actors[0].initialPose.Translation;
                    state.scaleLockedAtBaseline = interactable.scaleLocked;
                    if (state.initialVector.Length() >= kMinimumTwoHandDistance) {
                        QueueHaptic(state, HapticEvent::TwoHandStarted, first);
                        QueueHaptic(state, HapticEvent::TwoHandStarted, second);
                        return;
                    }
                    RebaselineSingle(state, transform, *input, first);
                }
            }

            const OVR::Posef currentPose = ActorPose(*input, first);
            const OVR::Posef delta = currentPose * first.initialPose.Inverted();
            OVR::Posef nextPose = delta * state.initialUserPose;
            const float stick = InteractionMath::ApplyDeadZone(
                    input->controllers[InputComponent::Right].joystick.y, kStickDeadZone);
            state.rayDistance = std::max(
                    kMinimumRayDistance, state.rayDistance + stick * kDepthSpeed * deltaSeconds);
            nextPose.Translation += RayDirection(ActorAim(*input, first)) *
                    (state.rayDistance - state.initialRayDistance);
            TransformSystem::SetPose(transform, transformState, nextPose);
            return;
        }

        const bool firstHeld = ActorHeld(*input, state.actors[0]);
        const bool secondHeld = ActorHeld(*input, state.actors[1]);
        if (!firstHeld || !secondHeld) {
            if (!firstHeld) QueueHaptic(state, HapticEvent::GrabReleased, state.actors[0]);
            if (!secondHeld) QueueHaptic(state, HapticEvent::GrabReleased, state.actors[1]);
            if (firstHeld) RebaselineSingle(state, transform, *input, state.actors[0]);
            else if (secondHeld) RebaselineSingle(state, transform, *input, state.actors[1]);
            else {
                state.mode = InteractionMode::Idle;
                state.selectedEntity = INVALID_ENTITY;
            }
            return;
        }

        const OVR::Vector3f firstPosition = ActorPose(*input, state.actors[0]).Translation;
        const OVR::Vector3f secondPosition = ActorPose(*input, state.actors[1]).Translation;
        const OVR::Vector3f currentVector = secondPosition - firstPosition;
        if (ScaleControl::NeedsRebaseline(
                state.scaleLockedAtBaseline, interactable.scaleLocked)) {
            state.initialUserPose = transform.modelPose;
            state.initialScale = transform.modelScale;
            state.initialMidpoint = (firstPosition + secondPosition) * 0.5f;
            state.initialVector = currentVector;
            state.scaleLockedAtBaseline = interactable.scaleLocked;
            state.scaleLimitLatched = false;
            return;
        }
        const float initialDistance = state.initialVector.Length();
        const float currentDistance = currentVector.Length();
        if (initialDistance < kMinimumTwoHandDistance || currentDistance < kMinimumTwoHandDistance) return;
        const float rawScale = state.initialScale.x * currentDistance / initialDistance;
        const float scale = ScaleControl::ResolveGestureScale(
            rawScale, transform.modelScale.x, interactable.scaleLocked,
            interactable.minimumScale, interactable.maximumScale);
        const bool clamped = !interactable.scaleLocked && scale != rawScale;
        if (clamped && !state.scaleLimitLatched) {
            QueueHaptic(state, HapticEvent::ScaleLimitReached, state.actors[0]);
            QueueHaptic(state, HapticEvent::ScaleLimitReached, state.actors[1]);
        }
        state.scaleLimitLatched = clamped;
        const OVR::Quatf rotationDelta = InteractionMath::AlignVectors(state.initialVector, currentVector);
        const OVR::Vector3f midpoint = (firstPosition + secondPosition) * 0.5f;
        const float scaleRatio = scale / state.initialScale.x;
        OVR::Posef nextPose(
                rotationDelta * state.initialUserPose.Rotation,
                midpoint + rotationDelta.Rotate(
                        state.initialUserPose.Translation - state.initialMidpoint) * scaleRatio);
        TransformSystem::SetPose(transform, transformState, nextPose);
        TransformSystem::SetScale(transform, transformState, {scale, scale, scale});
    });

    const HandInput& left = input->hands[InputComponent::Left];
    const HandInput& right = input->hands[InputComponent::Right];
    if (left.active && right.active &&
        PoseValid(left.joints[XR_HAND_JOINT_PALM_EXT]) && PoseValid(right.joints[XR_HAND_JOINT_PALM_EXT])) {
        const float distance = (left.palmPose.Translation - right.palmPose.Translation).Length();
        const OVR::Vector3f leftNormal = left.palmPose.Rotation.Rotate({0.0f, 0.0f, 1.0f});
        const OVR::Vector3f rightNormal = right.palmPose.Rotation.Rotate({0.0f, 0.0f, 1.0f});
        const bool facing = leftNormal.Dot(rightNormal) < -0.45f;
        const bool closing = inputState->previousPalmDistance > 0.0f &&
                inputState->previousPalmDistance - distance > kClapClosingDelta;
        if (inputState->clapArmed && facing && closing && distance < kClapCloseDistance &&
            !IsManipulating(ecs)) {
            input->uiToggleRequested = true;
            inputState->clapArmed = false;
        } else if (distance > kClapResetDistance) {
            inputState->clapArmed = true;
        }
        inputState->previousPalmDistance = distance;
    } else {
        inputState->previousPalmDistance = 0.0f;
    }
}
