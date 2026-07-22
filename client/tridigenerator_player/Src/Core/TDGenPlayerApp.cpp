#include <algorithm>
#include <fstream>
#include <iterator>
#include <iostream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <GLES3/gl3.h>
#include <glm/glm.hpp>
#include <openxr/openxr.h>
#include <curl/curl.h>

// Meta/SampleXrFramework
#include "GUI/VRMenuObject.h"
#include "Input/ControllerRenderer.h"
#include "Input/HandRenderer.h"
#include "Input/TinyUI.h"
#include "Render/BitmapFont.h"
#include "Render/GeometryBuilder.h"
//#include "Render/GeometryRenderer.h"
#include "Render/GlGeometry.h"
#include "Render/GlProgram.h"
#include "Render/GlTexture.h"
#include "Render/SimpleBeamRenderer.h"
#include "Render/SurfaceRender.h"

// Meta/OVR
#include "OVR_Math.h"
#include "OVR_FileSys.h"

// Meta/meta_openxr_preview
#include "meta_openxr_preview/openxr_oculus_helpers.h"

#define LOG_TAG "TDGenPlayerApp"
#include "../Core/Logging.h"

#include "TDGenPlayerApp.h"

#include "../Components/InputComponent.h"
#include "../Components/InteractableComponent.h"
#include "../Components/TransformComponent.h"
#include "../Components/FrameLoaderComponent.h"
#include "../Components/RenderComponent.h"
#include "../Components/UnlitGeometryRenderComponent.h"
#include "../Components/CameraLightEstimationComponent.h"

#include "../States/TransformState.h"
#include "../States/FrameLoaderState.h"
#include "../States/UnlitGeometryRenderState.h"
#include "../States/EnvironmentDepthState.h"
#include "../States/CameraLightEstimationState.h"
#include "../States/InputState.h"
#include "../States/InteractionState.h"

#include "../Systems/CoreSystem.h"
#include "../Systems/SceneSystem.h"
#include "../Systems/FrameLoaderSystem.h"
#include "../Systems/AudioSystem.h"
#include "../Systems/InputSystem.h"
#include "../Systems/InteractionSystem.h"
#include "../Systems/RenderSystem.h"
#include "../Systems/EnvironmentDepthSystem.h"
#include "../Systems/CameraLightEstimationSystem.h"
#include "../Systems/UnlitGeometryRenderSystem.h"

// All physical units in OpenXR are in meters, but sometimes it's more useful
// to think in cm, so this user defined literal converts from centimeters to meters
constexpr float operator"" _cm(long double centimeters)
{
    return static_cast<float>(centimeters * 0.01);
}
constexpr float operator"" _cm(unsigned long long centimeters)
{
    return static_cast<float>(centimeters * 0.01);
}

// For expressiveness; use _m rather than f literals when we mean meters
constexpr float operator"" _m(long double meters)
{
    return static_cast<float>(meters);
}
constexpr float operator"" _m(unsigned long long meters)
{
    return static_cast<float>(meters);
}

TDGenPlayerApp::TDGenPlayerApp() {
    BackgroundColor = OVR::Vector4f(0.0f, 0.0f, 0.0f, 0.0f);

    // Disable framework input management, letting this sample explicitly
    // call xrSyncActions() every frame; which includes control over which
    // ActionSet to set as active
    SkipInputHandling = false;

    curl_global_init(CURL_GLOBAL_ALL);
}

TDGenPlayerApp::~TDGenPlayerApp()
{
}

// Returns a list of OpenXR extensions requested for this app
// Note that the sample framework will filter out any extension
// that is not listed as supported.
std::vector<const char *> TDGenPlayerApp::GetExtensions()
{
    // Add extensions from XrApp
    std::vector<const char *> extensions = XrApp::GetExtensions();

    // Controller and hand input are used by the dataset picker.
    extensions.push_back(XR_FB_TOUCH_CONTROLLER_PRO_EXTENSION_NAME);
    extensions.push_back(XR_EXT_HAND_TRACKING_EXTENSION_NAME);
    extensions.push_back(XR_FB_HAND_TRACKING_MESH_EXTENSION_NAME);
    extensions.push_back(XR_FB_HAND_TRACKING_AIM_EXTENSION_NAME);

    // CoreSystem isn't constructed until AppInit(), but we must request any required
    // instance extensions up-front (during XrApp::CreateInstance()).
    extensions.push_back(XR_FB_PASSTHROUGH_EXTENSION_NAME);
    extensions.push_back(XR_FB_TRIANGLE_MESH_EXTENSION_NAME);
    extensions.push_back(XR_META_ENVIRONMENT_DEPTH_EXTENSION_NAME);
    extensions.push_back("XR_KHR_convert_timespec_time");

    return extensions;
}

std::unordered_map<XrPath, std::vector<XrActionSuggestedBinding>>
TDGenPlayerApp::GetSuggestedBindings(XrInstance instance) {
    auto bindings = OVRFW::XrApp::GetSuggestedBindings(instance);
    XrPath handPaths[2] = {LeftHandPath, RightHandPath};
    hapticAction_ = CreateAction(
            BaseActionSet, XR_ACTION_TYPE_VIBRATION_OUTPUT,
            "mesh_haptic", "Mesh interaction haptic", 2, handPaths);

    XrPath touchProfile = XR_NULL_PATH;
    XrPath touchProProfile = XR_NULL_PATH;
    XrPath touchPlusProfile = XR_NULL_PATH;
    xrStringToPath(instance, "/interaction_profiles/oculus/touch_controller", &touchProfile);
    xrStringToPath(instance, "/interaction_profiles/meta/touch_pro_controller", &touchProProfile);
    xrStringToPath(instance, "/interaction_profiles/meta/touch_plus_controller", &touchPlusProfile);
    bindings[touchProfile].emplace_back(
            ActionSuggestedBinding(hapticAction_, "/user/hand/left/output/haptic"));
    bindings[touchProfile].emplace_back(
            ActionSuggestedBinding(hapticAction_, "/user/hand/right/output/haptic"));
    bindings[touchProProfile] = bindings[touchProfile];
    bindings[touchPlusProfile] = bindings[touchProfile];
    return bindings;
}

// OVRFW::XrApp::Init() calls, among other things;
//  - xrInitializeLoaderKHR(...)
//  - xrCreateInstance with the extensions from GetExtensions(...),
//  - xrSuggestInteractionProfileBindings(...) to set up action bindings
// before calling the function below; AppInit():
bool TDGenPlayerApp::AppInit(const xrJava *context)
{
    OVRFW::XrApp::AppInit(context);

    // Initialize ECS and Systems
    entityManager_ = std::make_unique<EntityManager>();

    coreSystem_ = std::make_unique<CoreSystem>(GetInstance(), GetSystemId());
    sceneSystem_ = std::make_unique<SceneSystem>();
    frameLoaderSystem_ = std::make_unique<FrameLoaderSystem>();
    audioSystem_ = std::make_unique<AudioSystem>();
    inputSystem_ = std::make_unique<InputSystem>();
    interactionSystem_ = std::make_unique<InteractionSystem>();
    transformSystem_ = std::make_unique<TransformSystem>();
    renderSystem_ = std::make_unique<RenderSystem>();
    environmentDepthSystem_ = std::make_unique<EnvironmentDepthSystem>(GetInstance());
    cameraLightEstimationSystem_ = std::make_unique<CameraLightEstimationSystem>(GetInstance());
    unlitGeometryRenderSystem_ = std::make_unique<UnlitGeometryRenderSystem>();
    LOGI("ECS Systems Initialized");

    // Create entities

    // ---------- Create Core entity ----------
    auto CoreEntity = entityManager_->CreateEntity();
    entityManager_->AddComponent<CoreComponent>(CoreEntity, {});
    entityManager_->AddComponent<CoreState>(CoreEntity, {});
    entityManager_->AddComponent<EnvironmentDepthState>(CoreEntity, {});
    entityManager_->AddComponent<CameraLightEstimationComponent>(CoreEntity, {});
    entityManager_->AddComponent<CameraLightEstimationState>(CoreEntity, {});
    entityManager_->AddComponent<InputComponent>(CoreEntity, {});
    entityManager_->AddComponent<InputState>(CoreEntity, {});

    // ---------- Create Object entity ----------
    auto ObjectEntity = entityManager_->CreateEntity();
    objectEntity_ = ObjectEntity;
    TransformComponent transform;
    transform.modelPose = OVR::Posef(OVR::Quatf::Identity(), {0.0f, 0.0f, 0.0f});
    transform.modelScale = {1.0f, 1.0f, 1.0f};
    entityManager_->AddComponent<TransformComponent>(ObjectEntity, transform);
    entityManager_->AddComponent<TransformState>(ObjectEntity, {});
    entityManager_->AddComponent<InteractableComponent>(ObjectEntity, {});
    entityManager_->AddComponent<InteractionState>(ObjectEntity, {});
    entityManager_->AddComponent<FrameLoaderComponent>(ObjectEntity, {});
    entityManager_->AddComponent<FrameLoaderState>(ObjectEntity, {});
    entityManager_->AddComponent<UnlitGeometryRenderComponent>(ObjectEntity, {});
    entityManager_->AddComponent<UnlitGeometryRenderState>(ObjectEntity, {});

    // ---------- Initialize Systems ----------
    coreSystem_->Init(*entityManager_);
    sceneSystem_->Init(*entityManager_);
    frameLoaderSystem_->Init(*entityManager_);
    audioSystem_->Init(*entityManager_);
    inputSystem_->Init(*entityManager_);
    interactionSystem_->Init(*entityManager_);
    transformSystem_->Init(*entityManager_);
    renderSystem_->Init(*entityManager_);
    environmentDepthSystem_->Init(*entityManager_);
    cameraLightEstimationSystem_->Init(*entityManager_);
    unlitGeometryRenderSystem_->Init(*entityManager_);

    auto& initialLoader = entityManager_->GetComponent<FrameLoaderComponent>(objectEntity_);
    if (!initialLoader.selectedDatasetId.empty() && initialLoader.errorMessage.empty()) {
        auto& render = entityManager_->GetComponent<UnlitGeometryRenderComponent>(objectEntity_);
        render.maskVisibility_.Reset(initialLoader.dataset.maskLabels);
        BuildMaskSelector();
    } else {
        BuildDatasetPicker();
    }

    return true;
}

// XrApp::InitSession() calls:
// - xrCreateSession(...) to create our Session
// - xrCreateReferenceSpace(...) for local and stage space
// - Create swapchain with xrCreateSwapchain(...)
// - xrAttachSessionActionSets(...)
// before calling SessionInit():
bool TDGenPlayerApp::SessionInit()
{
    // Initialize XRInputActions and create action spaces using XrApp helper functions
    //xrInput_.Init(GetInstance(), GetSession());
    //xrInput_.CreateActionSpaces(GetLocalSpace());

    XrSession session = GetSession();
    const XrSpace appSpace = (GetCurrentSpace() != XR_NULL_HANDLE) ? GetCurrentSpace() : GetLocalSpace();
    coreSystem_->SetLocalSpace(*entityManager_, appSpace);
    coreSystem_->SessionInit(*entityManager_, session);
    inputSystem_->SessionInit(*entityManager_, session);
    environmentDepthSystem_->SessionInit(*entityManager_, session);
    cameraLightEstimationSystem_->SessionInit(*entityManager_, session);

    return true;
}

// The update function is called every frame before the Render() function.
// Some of the key OpenXR function called by the framework prior to calling this function:
//  - xrPollEvent(...)
//  - xrWaitFrame(...)
void TDGenPlayerApp::Update(const OVRFW::ovrApplFrameIn &in)
{
    using clock = std::chrono::steady_clock;
    double nowSeconds = std::chrono::duration<double>(clock::now().time_since_epoch()).count();

    // if SkipInputHandling is True, we need to sync action sets ourselves
    // --- xrSyncAction
    //
    // Sync action sets (including default set if not skipped by SkipInputHandling)
    //OXR(xrSyncActions(Session, &syncInfo));

    // Application logic update here

    coreSystem_->Update(*entityManager_);
    sceneSystem_->Update(*entityManager_);
    frameLoaderSystem_->Update(*entityManager_, nowSeconds);
    audioSystem_->Update(*entityManager_);
    inputSystem_->Update(*entityManager_, in);
    const float deltaSeconds = lastUpdateSeconds_ > 0.0
            ? static_cast<float>(std::clamp(nowSeconds - lastUpdateSeconds_, 0.0, 0.1))
            : 0.0f;
    lastUpdateSeconds_ = nowSeconds;
    interactionSystem_->Update(*entityManager_, deltaSeconds);
    entityManager_->ForEach<InteractionState>([&](EntityID, InteractionState& state) {
        for (const HapticRequest& request : state.hapticRequests) {
            DispatchHaptic(request.event, request.controllerMask);
        }
    });
    entityManager_->ForEach<InputComponent>([&](EntityID, InputComponent& input) {
        if (input.uiToggleRequested) {
            uiVisible_ = !uiVisible_;
            if (input.leftXPressedThisFrame) {
                DispatchHaptic(HapticEvent::UiToggled, 1u << InputComponent::Left);
            }
            input.uiToggleRequested = false;
        }
    });
    transformSystem_->Update(*entityManager_);
    renderSystem_->Update(*entityManager_);
    environmentDepthSystem_->Update(*entityManager_, in);
    cameraLightEstimationSystem_->Update(*entityManager_, in, Focused);
    unlitGeometryRenderSystem_->Update(*entityManager_, in);
#if defined(__ANDROID__)
    RefreshColorMatchingUi();
#endif
    if (ui_ && uiVisible_) {
        ui_->HitTestDevices().clear();
        entityManager_->ForEach<InputComponent>([&](EntityID, InputComponent& input) {
            for (size_t handIndex = 0; handIndex < input.hands.size(); ++handIndex) {
                const HandInput& hand = input.hands[handIndex];
                const ControllerInput& controller = input.controllers[handIndex];
                if (hand.active && hand.aimValid) {
                    ui_->AddHitTestRay(
                            hand.aimPose,
                            hand.indexPinching,
                            static_cast<int>(handIndex));
                } else if (controller.tracked) {
                    ui_->AddHitTestRay(
                            controller.aimPose,
                            controller.indexTrigger > 0.5f,
                            static_cast<int>(handIndex));
                }
            }
        });
        ui_->Update(in);
        if (uiRebuildPending_) {
            const UiMode nextMode = pendingUiMode_;
            uiRebuildPending_ = false;
            if (nextMode == UiMode::Masks) BuildMaskSelector();
            else if (nextMode == UiMode::ColorMatching) BuildColorMatchingControls();
            else BuildDatasetPicker();
        }
    }
}

void TDGenPlayerApp::AppRenderFrame(const OVRFW::ovrApplFrameIn& in, OVRFW::ovrRendererOutput& out)
{
    OVRFW::XrApp::AppRenderFrame(in, out);
    if (ui_ && uiVisible_) {
        ui_->Render(in, out);
    }
}

void TDGenPlayerApp::AppRenderEye(const OVRFW::ovrApplFrameIn& in, OVRFW::ovrRendererOutput& out, int eye)
{
    OVRFW::XrApp::AppRenderEye(in, out, eye);
}

// Called by the XrApp framework after the Update function
void TDGenPlayerApp::Render(const OVRFW::ovrApplFrameIn &in, OVRFW::ovrRendererOutput &out)
{
    unlitGeometryRenderSystem_->Render(*entityManager_, out.Surfaces);
    inputSystem_->Render(*entityManager_, out.Surfaces);
}

void TDGenPlayerApp::SessionEnd()
{
    StopHaptics();
    lastUpdateSeconds_ = 0.0;
    //xrInput_.Destroy();
    inputSystem_->SessionEnd(*entityManager_);
    cameraLightEstimationSystem_->SessionEnd(*entityManager_);
    environmentDepthSystem_->SessionEnd(*entityManager_);
    coreSystem_->SessionEnd(*entityManager_);
}

void TDGenPlayerApp::AppShutdown(const xrJava *context)
{
    ShutdownUi();
    // Explicitly destroy the systems and entity manager.
    // This is good practice to control the shutdown order.
    unlitGeometryRenderSystem_->Shutdown(*entityManager_);
    cameraLightEstimationSystem_->Shutdown(*entityManager_);
    renderSystem_->Shutdown(*entityManager_);
    transformSystem_->Shutdown(*entityManager_);
    interactionSystem_->Shutdown(*entityManager_);
    inputSystem_->Shutdown(*entityManager_);
    audioSystem_->Shutdown(*entityManager_);
    frameLoaderSystem_->Shutdown(*entityManager_);
    sceneSystem_->Shutdown(*entityManager_);
    coreSystem_->Shutdown(*entityManager_);
    environmentDepthSystem_->Shutdown(*entityManager_);

    unlitGeometryRenderSystem_.reset();
    cameraLightEstimationSystem_.reset();
    environmentDepthSystem_.reset();
    renderSystem_.reset();
    transformSystem_.reset();
    interactionSystem_.reset();
    inputSystem_.reset();
    audioSystem_.reset();
    frameLoaderSystem_.reset();
    sceneSystem_.reset();
    coreSystem_.reset();

    entityManager_.reset(); // Calls delete and empties the unique_ptr.
    hapticAction_ = XR_NULL_HANDLE;
    LOGI("ECS Systems Shutdown");

    curl_global_cleanup();

    OVRFW::XrApp::AppShutdown(context);
}

void TDGenPlayerApp::ShutdownUi() {
    if (ui_) {
        ui_->Shutdown();
        ui_.reset();
    }
    uiStatusLabel_ = nullptr;
}

void TDGenPlayerApp::BuildDatasetPicker() {
    ShutdownUi();
    ui_ = std::make_unique<OVRFW::TinyUI>();
    if (!ui_->Init(GetContext(), GetFileSys())) {
        LOGE("Failed to initialize dataset picker UI");
        ui_.reset();
        return;
    }
    currentUiMode_ = UiMode::Datasets;
    uiStatusLabel_ = ui_->AddLabel("ViPE datasets", {0.0f, 0.42f, -1.5f}, {500.0f, 70.0f});
#if defined(__ANDROID__)
    ui_->AddButton("Color matching", {0.0f, 0.31f, -1.5f}, {500.0f, 60.0f},
        [this]() { OpenColorMatchingControls(UiMode::Datasets); });
#endif
    auto& loader = entityManager_->GetComponent<FrameLoaderComponent>(objectEntity_);
    float y = 0.20f;
    for (const VipeCatalogEntry& entry : loader.catalog.datasets) {
        const std::string id = entry.id;
        ui_->AddButton(entry.displayName, {0.0f, y, -1.5f}, {500.0f, 70.0f},
            [this, id]() { SelectDataset(id); });
        y -= 0.12f;
    }
    if (loader.catalog.datasets.empty() && uiStatusLabel_) {
        const std::string status = loader.errorMessage.empty() ? "No datasets" : loader.errorMessage;
        uiStatusLabel_->SetText("%s", status.c_str());
    }
}

void TDGenPlayerApp::BuildMaskSelector() {
    ShutdownUi();
    ui_ = std::make_unique<OVRFW::TinyUI>();
    if (!ui_->Init(GetContext(), GetFileSys())) {
        LOGE("Failed to initialize mask selector UI");
        ui_.reset();
        return;
    }
    currentUiMode_ = UiMode::Masks;

    auto& loader = entityManager_->GetComponent<FrameLoaderComponent>(objectEntity_);
    auto& render = entityManager_->GetComponent<UnlitGeometryRenderComponent>(objectEntity_);
    ui_->AddLabel("Masks: " + loader.selectedDatasetId,
        {0.0f, 0.48f, -1.5f}, {620.0f, 60.0f});
    ui_->AddButton("Back to datasets", {0.0f, 0.37f, -1.5f}, {620.0f, 60.0f},
        [this]() { RequestUiMode(UiMode::Datasets); });
#if defined(__ANDROID__)
    ui_->AddButton("Color matching", {0.0f, 0.27f, -1.5f}, {620.0f, 60.0f},
        [this]() { OpenColorMatchingControls(UiMode::Masks); });
#endif

    for (int id = 0; id < 256; ++id) {
        maskToggleValues_[static_cast<size_t>(id)] =
            render.maskVisibility_.IsVisible(static_cast<uint8_t>(id));
    }
    float y = 0.16f;
    for (const MaskVisibilityEntry& entry : render.maskVisibility_.Entries()) {
        const uint8_t id = entry.id;
        const std::string suffix = std::to_string(static_cast<unsigned int>(id)) +
            " - " + entry.label;
        ui_->AddToggleButton("Visible: " + suffix, "Hidden: " + suffix,
            &maskToggleValues_[id], {0.0f, y, -1.5f}, {620.0f, 60.0f},
            [this, id]() {
                auto& component = entityManager_->GetComponent<UnlitGeometryRenderComponent>(objectEntity_);
                component.maskVisibility_.SetVisible(id, maskToggleValues_[id]);
            });
        y -= 0.10f;
    }
}

void TDGenPlayerApp::BuildColorMatchingControls() {
    ShutdownUi();
    ui_ = std::make_unique<OVRFW::TinyUI>();
    if (!ui_->Init(GetContext(), GetFileSys())) {
        LOGE("Failed to initialize color matching UI");
        ui_.reset();
        return;
    }
    currentUiMode_ = UiMode::ColorMatching;

    CameraLightEstimationComponent* component = nullptr;
    CameraLightEstimationState* state = nullptr;
    entityManager_->ForEachMulti<CameraLightEstimationComponent, CameraLightEstimationState>(
        [&](EntityID, CameraLightEstimationComponent& c, CameraLightEstimationState& s) {
            component = &c;
            state = &s;
        });
    if (!component || !state) {
        colorMatchingUiSnapshotValid_ = false;
        ui_->AddLabel("Color matching unavailable", {0.0f, 0.35f, -1.5f}, {620.0f, 70.0f});
        ui_->AddButton("Back", {0.0f, 0.22f, -1.5f}, {620.0f, 60.0f},
            [this]() { RequestUiMode(colorMatchingReturnMode_); });
        return;
    }
    colorMatchingUiSnapshotValid_ = true;
    colorMatchingUiRequested_ = component->requestedTier;
    colorMatchingUiActive_ = state->tier;
    colorMatchingUiGlobal_ = state->globalAvailability;
    colorMatchingUiSpatial_ = state->spatialAvailability;

    ui_->AddLabel("Color matching", {0.0f, 0.48f, -1.5f}, {620.0f, 60.0f});
    const char* active = state->tier == LightEstimateTier::Spatial ? "Spatial" :
        state->tier == LightEstimateTier::Global ? "Global" : "Unavailable";
    const std::string status = std::string("Selected: ") +
        ColorMatchingTierName(component->requestedTier) + " | Active: " + active;
    uiStatusLabel_ = ui_->AddLabel(status, {0.0f, 0.38f, -1.5f}, {620.0f, 55.0f});
    ui_->AddButton("Back", {0.0f, 0.27f, -1.5f}, {620.0f, 55.0f},
        [this]() { RequestUiMode(colorMatchingReturnMode_); });

    const auto addTierRow = [&](ColorMatchingTier tier, TierAvailability availability, float y) {
        const std::string name = ColorMatchingTierName(tier);
        if (component->requestedTier == tier) {
            ui_->AddLabel(name + " (Selected)", {0.0f, y, -1.5f}, {620.0f, 60.0f});
        } else if (!IsTierSelectable(
                       tier, state->globalAvailability, state->spatialAvailability)) {
            ui_->AddLabel(name + " (" + TierAvailabilityName(availability) + ")",
                {0.0f, y, -1.5f}, {620.0f, 60.0f});
        } else {
            ui_->AddButton(name, {0.0f, y, -1.5f}, {620.0f, 60.0f},
                [this, tier]() { SelectColorMatchingTier(tier); });
        }
    };
    addTierRow(ColorMatchingTier::Disabled, TierAvailability::Available, 0.15f);
    addTierRow(ColorMatchingTier::Global, state->globalAvailability, 0.04f);
    addTierRow(ColorMatchingTier::Spatial, state->spatialAvailability, -0.07f);
}

void TDGenPlayerApp::OpenColorMatchingControls(UiMode returnMode) {
    colorMatchingReturnMode_ = returnMode;
    RequestUiMode(UiMode::ColorMatching);
}

void TDGenPlayerApp::SelectColorMatchingTier(ColorMatchingTier tier) {
    entityManager_->ForEachMulti<CameraLightEstimationComponent, CameraLightEstimationState>(
        [&](EntityID, CameraLightEstimationComponent& component, CameraLightEstimationState& state) {
            if (!IsTierSelectable(tier, state.globalAvailability, state.spatialAvailability)) return;
            component.requestedTier = tier;
        });
    RequestUiMode(UiMode::ColorMatching);
}

void TDGenPlayerApp::RefreshColorMatchingUi() {
    if (currentUiMode_ != UiMode::ColorMatching || uiRebuildPending_) return;
    entityManager_->ForEachMulti<CameraLightEstimationComponent, CameraLightEstimationState>(
        [&](EntityID, CameraLightEstimationComponent& component, CameraLightEstimationState& state) {
            if (!colorMatchingUiSnapshotValid_ ||
                component.requestedTier != colorMatchingUiRequested_ ||
                state.tier != colorMatchingUiActive_ ||
                state.globalAvailability != colorMatchingUiGlobal_ ||
                state.spatialAvailability != colorMatchingUiSpatial_) {
                colorMatchingUiRequested_ = component.requestedTier;
                colorMatchingUiActive_ = state.tier;
                colorMatchingUiGlobal_ = state.globalAvailability;
                colorMatchingUiSpatial_ = state.spatialAvailability;
                colorMatchingUiSnapshotValid_ = true;
                RequestUiMode(UiMode::ColorMatching);
            }
        });
}

void TDGenPlayerApp::RequestUiMode(UiMode mode) {
    pendingUiMode_ = mode;
    uiRebuildPending_ = true;
}

void TDGenPlayerApp::SelectDataset(const std::string& datasetId) {
    auto& loader = entityManager_->GetComponent<FrameLoaderComponent>(objectEntity_);
    auto& loaderState = entityManager_->GetComponent<FrameLoaderState>(objectEntity_);
    unlitGeometryRenderSystem_->Shutdown(*entityManager_);
    const bool selected = frameLoaderSystem_->SelectDataset(datasetId, loader, loaderState);
    if (selected) {
        auto& render = entityManager_->GetComponent<UnlitGeometryRenderComponent>(objectEntity_);
        render.maskVisibility_.Reset(loader.dataset.maskLabels);
        unlitGeometryRenderSystem_->Init(*entityManager_);
        RequestUiMode(UiMode::Masks);
    } else {
        unlitGeometryRenderSystem_->Init(*entityManager_);
        if (uiStatusLabel_) {
            uiStatusLabel_->SetText("Load failed: %s", loader.errorMessage.c_str());
        }
    }
}

void TDGenPlayerApp::DispatchHaptic(HapticEvent event, uint8_t controllerMask) {
    if (!Focused || GetSession() == XR_NULL_HANDLE || hapticAction_ == XR_NULL_HANDLE ||
        controllerMask == 0) return;

    float durationSeconds = 0.025f;
    float amplitude = 0.25f;
    switch (event) {
        case HapticEvent::GrabAccepted: durationSeconds = 0.035f; amplitude = 0.30f; break;
        case HapticEvent::TwoHandStarted: durationSeconds = 0.050f; amplitude = 0.45f; break;
        case HapticEvent::GrabReleased: durationSeconds = 0.020f; amplitude = 0.18f; break;
        case HapticEvent::ScaleLimitReached: durationSeconds = 0.070f; amplitude = 0.65f; break;
        case HapticEvent::UiToggled: durationSeconds = 0.030f; amplitude = 0.32f; break;
    }

    XrHapticVibration vibration{XR_TYPE_HAPTIC_VIBRATION};
    vibration.duration = ToXrTime(durationSeconds);
    vibration.frequency = XR_FREQUENCY_UNSPECIFIED;
    vibration.amplitude = amplitude;
    const XrPath paths[2] = {LeftHandPath, RightHandPath};
    for (size_t side = 0; side < 2; ++side) {
        if ((controllerMask & (1u << side)) == 0) continue;
        XrHapticActionInfo info{XR_TYPE_HAPTIC_ACTION_INFO};
        info.action = hapticAction_;
        info.subactionPath = paths[side];
        const XrResult result = xrApplyHapticFeedback(
                GetSession(), &info,
                reinterpret_cast<const XrHapticBaseHeader*>(&vibration));
        if (XR_FAILED(result)) LOGW("xrApplyHapticFeedback failed for side %zu: %d", side, result);
    }
}

void TDGenPlayerApp::StopHaptics() {
    if (GetSession() == XR_NULL_HANDLE || hapticAction_ == XR_NULL_HANDLE) return;
    const XrPath paths[2] = {LeftHandPath, RightHandPath};
    for (size_t side = 0; side < 2; ++side) {
        XrHapticActionInfo info{XR_TYPE_HAPTIC_ACTION_INFO};
        info.action = hapticAction_;
        info.subactionPath = paths[side];
        const XrResult result = xrStopHapticFeedback(GetSession(), &info);
        if (XR_FAILED(result)) {
            LOGW("xrStopHapticFeedback failed for side %zu: %d", side, result);
        }
    }
}
// Insert passthrough layer before projection layers when available
void TDGenPlayerApp::PreProjectionAddLayer(xrCompositorLayerUnion* layers, int& layerCount) {
    XrCompositionLayerPassthroughFB passthroughLayer{XR_TYPE_COMPOSITION_LAYER_PASSTHROUGH_FB};
    if (coreSystem_->BuildPassthroughLayer(*entityManager_, passthroughLayer, XR_NULL_HANDLE)) {
        layers[layerCount++].Passthrough = passthroughLayer;
    }
}
