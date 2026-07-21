#include "InputSystem.h"

#include <vector>

#include "meta_openxr_preview/openxr_oculus_helpers.h"

#include "../Components/CoreComponent.h"
#include "../Components/InputComponent.h"
#include "../States/CoreState.h"
#include "../States/InputState.h"

#define LOG_TAG "InputSystem"
#include "../Core/Logging.h"

namespace {

constexpr size_t kHandCount = 2;

XrHandEXT HandForIndex(size_t handIndex) {
    return handIndex == InputComponent::Left ? XR_HAND_LEFT_EXT : XR_HAND_RIGHT_EXT;
}

bool IsPoseValid(XrSpaceLocationFlags flags) {
    constexpr XrSpaceLocationFlags required =
            XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
    return (flags & required) == required;
}

OVR::Posef ToOvrPose(const XrPosef& pose) {
    return OVR::Posef(
            OVR::Quatf(
                    pose.orientation.x,
                    pose.orientation.y,
                    pose.orientation.z,
                    pose.orientation.w),
            OVR::Vector3f(pose.position.x, pose.position.y, pose.position.z));
}

bool InitializeHandRenderer(
        CoreState& coreState,
        InputState& inputState,
        size_t handIndex) {
    XrHandTrackingMeshFB mesh{XR_TYPE_HAND_TRACKING_MESH_FB};
    XrResult result = coreState.xrGetHandMeshFB_(inputState.handTrackers[handIndex], &mesh);
    if (XR_FAILED(result)) {
        LOGW("xrGetHandMeshFB size query failed for hand %zu: %d", handIndex, result);
        return false;
    }

    std::vector<XrPosef> jointBindPoses(mesh.jointCountOutput);
    std::vector<float> jointRadii(mesh.jointCountOutput);
    std::vector<XrHandJointEXT> jointParents(mesh.jointCountOutput);
    std::vector<XrVector3f> vertexPositions(mesh.vertexCountOutput);
    std::vector<XrVector3f> vertexNormals(mesh.vertexCountOutput);
    std::vector<XrVector2f> vertexUVs(mesh.vertexCountOutput);
    std::vector<XrVector4sFB> vertexBlendIndices(mesh.vertexCountOutput);
    std::vector<XrVector4f> vertexBlendWeights(mesh.vertexCountOutput);
    std::vector<int16_t> indices(mesh.indexCountOutput);

    mesh.jointCapacityInput = mesh.jointCountOutput;
    mesh.jointBindPoses = jointBindPoses.data();
    mesh.jointRadii = jointRadii.data();
    mesh.jointParents = jointParents.data();
    mesh.vertexCapacityInput = mesh.vertexCountOutput;
    mesh.vertexPositions = vertexPositions.data();
    mesh.vertexNormals = vertexNormals.data();
    mesh.vertexUVs = vertexUVs.data();
    mesh.vertexBlendIndices = vertexBlendIndices.data();
    mesh.vertexBlendWeights = vertexBlendWeights.data();
    mesh.indexCapacityInput = mesh.indexCountOutput;
    mesh.indices = indices.data();

    result = coreState.xrGetHandMeshFB_(inputState.handTrackers[handIndex], &mesh);
    if (XR_FAILED(result)) {
        LOGW("xrGetHandMeshFB data query failed for hand %zu: %d", handIndex, result);
        return false;
    }

    return inputState.handRenderers[handIndex].Init(
            &mesh, handIndex == InputComponent::Left);
}

} // namespace

bool InputSystem::Init(EntityManager& ecs) {
    ecs.ForEach<InputComponent>([](EntityID, InputComponent& input) { input = {}; });
    return true;
}

bool InputSystem::SessionInit(EntityManager& ecs, XrSession session) {
    bool initialized = true;
    ecs.ForEachMulti<CoreComponent, CoreState, InputComponent, InputState>(
            [&](EntityID,
                CoreComponent& core,
                CoreState& coreState,
                InputComponent& input,
                InputState& state) {
        if (state.sessionInitialized) {
            SessionEnd(ecs);
        }

        input = {};
        for (size_t handIndex = 0; handIndex < kHandCount; ++handIndex) {
            const bool isLeft = handIndex == InputComponent::Left;
            state.controllerRendererInitialized[handIndex] =
                    state.controllerRenderers[handIndex].Init(isLeft);
            if (!state.controllerRendererInitialized[handIndex]) {
                LOGW("Controller renderer initialization failed for hand %zu", handIndex);
                initialized = false;
            }
        }

        if (core.supportsHandTracking && coreState.xrCreateHandTrackerEXT_) {
            for (size_t handIndex = 0; handIndex < kHandCount; ++handIndex) {
                XrHandTrackerCreateInfoEXT createInfo{XR_TYPE_HAND_TRACKER_CREATE_INFO_EXT};
                createInfo.hand = HandForIndex(handIndex);
                createInfo.handJointSet = XR_HAND_JOINT_SET_DEFAULT_EXT;
                const XrResult result = coreState.xrCreateHandTrackerEXT_(
                        session, &createInfo, &state.handTrackers[handIndex]);
                if (XR_FAILED(result)) {
                    LOGW("xrCreateHandTrackerEXT failed for hand %zu: %d", handIndex, result);
                    state.handTrackers[handIndex] = XR_NULL_HANDLE;
                    continue;
                }

                if (core.supportsHandTrackingMesh && coreState.xrGetHandMeshFB_) {
                    state.handRendererInitialized[handIndex] =
                            InitializeHandRenderer(coreState, state, handIndex);
                    if (!state.handRendererInitialized[handIndex]) {
                        LOGW("Hand renderer initialization failed for hand %zu", handIndex);
                    }
                }
            }
        }
        state.sessionInitialized = true;
    });
    return initialized;
}

void InputSystem::SessionEnd(EntityManager& ecs) {
    ecs.ForEachMulti<CoreState, InputComponent, InputState>(
            [](EntityID, CoreState& coreState, InputComponent& input, InputState& state) {
        for (size_t handIndex = 0; handIndex < kHandCount; ++handIndex) {
            if (state.handRendererInitialized[handIndex]) {
                state.handRenderers[handIndex].Shutdown();
                state.handRendererInitialized[handIndex] = false;
            }
            if (state.controllerRendererInitialized[handIndex]) {
                state.controllerRenderers[handIndex].Shutdown();
                state.controllerRendererInitialized[handIndex] = false;
            }
            if (state.handTrackers[handIndex] != XR_NULL_HANDLE &&
                coreState.xrDestroyHandTrackerEXT_) {
                const XrResult result =
                        coreState.xrDestroyHandTrackerEXT_(state.handTrackers[handIndex]);
                if (XR_FAILED(result)) {
                    LOGW("xrDestroyHandTrackerEXT failed for hand %zu: %d", handIndex, result);
                }
            }
            state.handTrackers[handIndex] = XR_NULL_HANDLE;
            state.previousPinch[handIndex] = false;
        }
        input = {};
        state.sessionInitialized = false;
    });
}

void InputSystem::Shutdown(EntityManager& ecs) {
    SessionEnd(ecs);
}

void InputSystem::Update(EntityManager& ecs, const OVRFW::ovrApplFrameIn& in) {
    ecs.ForEachMulti<CoreComponent, CoreState, InputComponent, InputState>(
            [&](EntityID,
                CoreComponent& core,
                CoreState& coreState,
                InputComponent& input,
                InputState& state) {
        input.buttons = in.AllButtons;
        input.lastButtons = in.LastFrameAllButtons;
        input.touches = in.AllTouches;
        input.lastTouches = in.LastFrameAllTouches;
        input.leftXPressedThisFrame =
                (input.buttons & OVRFW::ovrApplFrameIn::kButtonX) != 0 &&
                (input.lastButtons & OVRFW::ovrApplFrameIn::kButtonX) == 0;
        input.uiToggleRequested = input.leftXPressedThisFrame;

        input.controllers[InputComponent::Left] = {
                in.LeftRemoteTracked,
                in.LeftRemotePose,
                in.LeftRemotePointPose,
                in.LeftRemoteJoystick,
                in.LeftRemoteIndexTrigger,
                in.LeftRemoteGripTrigger,
                in.LeftRemoteIndexClick};
        input.controllers[InputComponent::Right] = {
                in.RightRemoteTracked,
                in.RightRemotePose,
                in.RightRemotePointPose,
                in.RightRemoteJoystick,
                in.RightRemoteIndexTrigger,
                in.RightRemoteGripTrigger,
                in.RightRemoteIndexClick};

        for (size_t handIndex = 0; handIndex < kHandCount; ++handIndex) {
            ControllerInput& controller = input.controllers[handIndex];
            controller.gripPressed = controller.gripTrigger >= 0.55f;
            controller.gripPressedThisFrame =
                    controller.gripPressed && !state.previousGrip[handIndex];
            controller.gripReleasedThisFrame =
                    !controller.gripPressed && state.previousGrip[handIndex];
            state.previousGrip[handIndex] = controller.gripPressed;
        }

        for (size_t handIndex = 0; handIndex < kHandCount; ++handIndex) {
            HandInput& hand = input.hands[handIndex];
            state.previousPinch[handIndex] = hand.indexPinching;
            hand.active = false;
            hand.aimValid = false;
            hand.indexPinching = false;
            hand.pinchPressedThisFrame = false;
            hand.pinchReleasedThisFrame = false;

            if (!state.sessionInitialized || !core.supportsHandTracking ||
                !coreState.xrLocateHandJointsEXT_ ||
                state.handTrackers[handIndex] == XR_NULL_HANDLE ||
                coreState.localSpace == XR_NULL_HANDLE) {
                continue;
            }

            XrHandTrackingAimStateFB aimState{XR_TYPE_HAND_TRACKING_AIM_STATE_FB};
            XrHandJointLocationsEXT locations{XR_TYPE_HAND_JOINT_LOCATIONS_EXT};
            locations.next = core.supportsHandTrackingAim ? &aimState : nullptr;
            locations.jointCount = static_cast<uint32_t>(hand.joints.size());
            locations.jointLocations = hand.joints.data();

            XrHandJointsLocateInfoEXT locateInfo{XR_TYPE_HAND_JOINTS_LOCATE_INFO_EXT};
            locateInfo.baseSpace = coreState.localSpace;
            locateInfo.time = ToXrTime(in.PredictedDisplayTime);
            const XrResult result = coreState.xrLocateHandJointsEXT_(
                    state.handTrackers[handIndex], &locateInfo, &locations);
            if (XR_FAILED(result) || !locations.isActive) {
                continue;
            }

            hand.active = IsPoseValid(
                    hand.joints[XR_HAND_JOINT_PALM_EXT].locationFlags);
            if (!hand.active) {
                continue;
            }
            hand.palmPose = ToOvrPose(hand.joints[XR_HAND_JOINT_PALM_EXT].pose);

            if (core.supportsHandTrackingAim &&
                (aimState.status & XR_HAND_TRACKING_AIM_VALID_BIT_FB) != 0) {
                hand.aimValid = true;
                hand.aimPose = ToOvrPose(aimState.aimPose);
                hand.indexPinching =
                        (aimState.status & XR_HAND_TRACKING_AIM_INDEX_PINCHING_BIT_FB) != 0;
            }
            hand.pinchPressedThisFrame =
                    hand.indexPinching && !state.previousPinch[handIndex];
            hand.pinchReleasedThisFrame =
                    !hand.indexPinching && state.previousPinch[handIndex];

            if (state.handRendererInitialized[handIndex]) {
                state.handRenderers[handIndex].Update(hand.joints.data());
            }
        }

        for (size_t handIndex = 0; handIndex < kHandCount; ++handIndex) {
            const ControllerInput& controller = input.controllers[handIndex];
            if (state.controllerRendererInitialized[handIndex] && controller.tracked &&
                !input.hands[handIndex].active) {
                state.controllerRenderers[handIndex].Update(controller.gripPose);
            }
        }
    });
}

void InputSystem::Render(
        EntityManager& ecs,
        std::vector<OVRFW::ovrDrawSurface>& surfaces) {
    ecs.ForEachMulti<InputComponent, InputState>(
            [&](EntityID, InputComponent& input, InputState& state) {
        for (size_t handIndex = 0; handIndex < kHandCount; ++handIndex) {
            if (input.hands[handIndex].active &&
                state.handRendererInitialized[handIndex]) {
                state.handRenderers[handIndex].Render(surfaces);
            } else if (input.controllers[handIndex].tracked &&
                       state.controllerRendererInitialized[handIndex]) {
                state.controllerRenderers[handIndex].Render(surfaces);
            }
        }
    });
}
