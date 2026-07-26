#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>
#include <iostream>
#include <iomanip>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#if defined(__ANDROID__)
#include <jni.h>
#endif

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
#include "../Components/ColorMatchingSettings.h"
#include "../Components/MeshDetailSettings.h"

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
#include "../Systems/ScaleControl.h"

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

#if defined(__ANDROID__)
namespace {

GLuint CreateCircularButtonTexture(const bool showPlayIcon) {
    constexpr int textureSize = 64;
    constexpr int samplesPerAxis = 4;
    constexpr float outerRadius = 30.0f;
    constexpr float innerRadius = 26.5f;
    constexpr float fillColor[3] = {0.31f, 0.24f, 0.82f};
    constexpr float borderColor[3] = {0.86f, 0.92f, 1.0f};
    constexpr float iconColor[3] = {1.0f, 1.0f, 1.0f};
    std::vector<uint8_t> pixels(textureSize * textureSize * 4);

    for (int y = 0; y < textureSize; ++y) {
        for (int x = 0; x < textureSize; ++x) {
            float accumulatedColor[3] = {};
            int coveredSamples = 0;
            for (int sampleY = 0; sampleY < samplesPerAxis; ++sampleY) {
                for (int sampleX = 0; sampleX < samplesPerAxis; ++sampleX) {
                    const float dx =
                        static_cast<float>(x) +
                        (static_cast<float>(sampleX) + 0.5f) / samplesPerAxis -
                        textureSize * 0.5f;
                    const float dy =
                        static_cast<float>(y) +
                        (static_cast<float>(sampleY) + 0.5f) / samplesPerAxis -
                        textureSize * 0.5f;
                    const float distance = std::sqrt(dx * dx + dy * dy);
                    if (distance > outerRadius) {
                        continue;
                    }

                    const bool isIcon = showPlayIcon
                        ? (std::abs(dy) <= 10.0f &&
                           dx >= -6.5f &&
                           dx <= 10.5f - 1.7f * std::abs(dy))
                        : (std::abs(dy) <= 9.0f &&
                           (std::abs(dx - 5.0f) <= 2.25f ||
                            std::abs(dx + 5.0f) <= 2.25f));
                    const float* color = isIcon
                        ? iconColor
                        : distance >= innerRadius ? borderColor : fillColor;
                    for (int channel = 0; channel < 3; ++channel) {
                        accumulatedColor[channel] += color[channel];
                    }
                    ++coveredSamples;
                }
            }

            const size_t offset = static_cast<size_t>((y * textureSize + x) * 4);
            if (coveredSamples == 0) {
                pixels[offset + 0] = 0;
                pixels[offset + 1] = 0;
                pixels[offset + 2] = 0;
                pixels[offset + 3] = 0;
                continue;
            }
            for (int channel = 0; channel < 3; ++channel) {
                pixels[offset + channel] = static_cast<uint8_t>(
                    accumulatedColor[channel] * 255.0f / coveredSamples + 0.5f);
            }
            pixels[offset + 3] = static_cast<uint8_t>(
                coveredSamples * 255.0f /
                (samplesPerAxis * samplesPerAxis) + 0.5f);
        }
    }

    GLuint texture = 0;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(
        GL_TEXTURE_2D,
        0,
        GL_RGBA8,
        textureSize,
        textureSize,
        0,
        GL_RGBA,
        GL_UNSIGNED_BYTE,
        pixels.data());
    glBindTexture(GL_TEXTURE_2D, 0);
    return texture;
}

} // namespace
#endif

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
#if defined(__ANDROID__)
    extensions.push_back(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);
#endif

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
    InitDisplayRefreshRateExtension();

    // Initialize ECS and Systems
    entityManager_ = std::make_unique<EntityManager>();
    gpuTiming_ = std::make_unique<GpuTimingManager>(performanceTimingStats_);
    if (!gpuTiming_->Init()) {
        LOGW("GPU performance timing unavailable; CPU diagnostics remain enabled");
    }

    coreSystem_ = std::make_unique<CoreSystem>(GetInstance(), GetSystemId());
    sceneSystem_ = std::make_unique<SceneSystem>();
    frameLoaderSystem_ =
        std::make_unique<FrameLoaderSystem>(performanceTimingStats_);
    audioSystem_ = std::make_unique<AudioSystem>();
    inputSystem_ = std::make_unique<InputSystem>();
    interactionSystem_ = std::make_unique<InteractionSystem>();
    transformSystem_ = std::make_unique<TransformSystem>();
    renderSystem_ = std::make_unique<RenderSystem>();
    environmentDepthSystem_ = std::make_unique<EnvironmentDepthSystem>(GetInstance());
    cameraLightEstimationSystem_ = std::make_unique<CameraLightEstimationSystem>(
        GetInstance(), performanceTimingStats_, gpuTiming_.get());
    unlitGeometryRenderSystem_ = std::make_unique<UnlitGeometryRenderSystem>(
        performanceTimingStats_, gpuTiming_.get());
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

#if defined(__ANDROID__)
    LoadMeshDetailSettings();
    entityManager_->GetComponent<FrameLoaderComponent>(objectEntity_)
        .meshDetailDivisor.store(
            meshDetailSaved_.divisor, std::memory_order_release);
#endif

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
    unlitGeometryRenderSystem_->Init(*entityManager_, meshDetailSaved_.divisor);
    pointerRenderer_ = std::make_unique<OVRFW::SimpleBeamRenderer>();
    pointerRenderer_->Init(
        GetFileSys(), nullptr, OVR::Vector4f(0.35f, 0.75f, 1.0f, 1.0f), 1.0f, true);

    auto& initialLoader = entityManager_->GetComponent<FrameLoaderComponent>(objectEntity_);
    initialLoader.dynamicIndexCullingEnabled.store(
        dynamicIndexCullingEnabled_, std::memory_order_release);
#if defined(__ANDROID__)
    LoadColorMatchingSettingsForDataset();
#endif
    if (!initialLoader.selectedDatasetId.empty() && initialLoader.errorMessage.empty()) {
        auto& render = entityManager_->GetComponent<UnlitGeometryRenderComponent>(objectEntity_);
        render.maskVisibility_.Reset(initialLoader.dataset.maskLabels);
        initialLoader.maskVisibilityPublisher->Publish(
            render.maskVisibility_.ShaderValues());
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
    frameTimingStats_.Reset();
    xrFrameTimingStats_.Reset();
    performanceTimingStats_->Reset();
    if (gpuTiming_) gpuTiming_->Init();
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
    InitDisplayRefreshRateSession();

    return true;
}

void TDGenPlayerApp::AppHandleEvent(XrEventDataBaseHeader* baseEventHeader) {
    if (!baseEventHeader ||
        baseEventHeader->type !=
            XR_TYPE_EVENT_DATA_DISPLAY_REFRESH_RATE_CHANGED_FB) {
        return;
    }

    const auto* event =
        reinterpret_cast<const XrEventDataDisplayRefreshRateChangedFB*>(
            baseEventHeader);
    currentPanelRefreshRate_ = event->toDisplayRefreshRate;
    currentPanelRefreshRateValid_ =
        std::isfinite(currentPanelRefreshRate_) &&
        currentPanelRefreshRate_ > 0.0f;
    frameTimingStats_.SetPanelRefreshRate(
        currentPanelRefreshRateValid_ ? currentPanelRefreshRate_ : 0.0);
    xrFrameTimingStats_.SetPanelRefreshRate(
        currentPanelRefreshRateValid_ ? currentPanelRefreshRate_ : 0.0);
    std::ostringstream message;
    message << std::fixed << std::setprecision(1)
            << "Panel changed " << event->fromDisplayRefreshRate
            << " -> " << event->toDisplayRefreshRate << " Hz";
    displayRefreshRateMessage_ = message.str();
    LOGI(
        "Display refresh rate changed from %.1f Hz to %.1f Hz",
        event->fromDisplayRefreshRate,
        event->toDisplayRefreshRate);
    RefreshDisplayRefreshRateUi();
    RefreshDiagnosticOverlay();
}

// The update function is called every frame before the Render() function.
// Some of the key OpenXR function called by the framework prior to calling this function:
//  - xrPollEvent(...)
//  - xrWaitFrame(...)
void TDGenPlayerApp::Update(const OVRFW::ovrApplFrameIn &in)
{
    using clock = std::chrono::steady_clock;
    const auto now = clock::now();
    performanceTimingStats_->SetEnabled(diagnosticOverlayVisible_);
    performanceTimingStats_->BeginForegroundFrame();
    if (gpuTiming_) gpuTiming_->Poll();
    double nowSeconds = std::chrono::duration<double>(now.time_since_epoch()).count();
    // Refresh the large TinyUI diagnostic label only when the performance
    // window publishes below. Rebuilding it once here and again at the end of
    // the same frame causes avoidable periodic GL-thread hitches.
    frameTimingStats_.AddFrame(now);
    // This timestamp comes directly from xrWaitFrame and cannot be tied to
    // media timestamps. Reporting both clocks distinguishes compositor
    // throttling from a wall-clock diagnostic anomaly.
    if (std::isfinite(in.PredictedDisplayTime) &&
        in.PredictedDisplayTime > 0.0) {
        const auto xrTimestamp =
            FrameTimingStats::TimePoint{} +
            std::chrono::duration_cast<FrameTimingStats::Clock::duration>(
                std::chrono::duration<double>(in.PredictedDisplayTime));
        xrFrameTimingStats_.AddFrame(xrTimestamp);
    }

    // if SkipInputHandling is True, we need to sync action sets ourselves
    // --- xrSyncAction
    //
    // Sync action sets (including default set if not skipped by SkipInputHandling)
    //OXR(xrSyncActions(Session, &syncInfo));

    // Application logic update here

    coreSystem_->Update(*entityManager_);
    sceneSystem_->Update(*entityManager_);
    inputSystem_->Update(*entityManager_, in);
    if (!uiAnchorInitialized_) {
        uiAnchorPose_ = in.HeadPose;
        uiAnchorInitialized_ = true;
        const auto& loader =
            entityManager_->GetComponent<FrameLoaderComponent>(objectEntity_);
        if (!loader.selectedDatasetId.empty() && loader.errorMessage.empty()) {
            BuildMaskSelector();
        } else {
            BuildDatasetPicker();
        }
#if defined(__ANDROID__)
        BuildPlaybackControls();
        BuildDiagnosticOverlay();
#endif
    }
    entityManager_->ForEach<InputComponent>([&](EntityID, InputComponent& input) {
        if (Focused && input.rightAPressedThisFrame) {
            TogglePlayback();
            input.rightAPressedThisFrame = false;
        }
    });
    frameLoaderSystem_->Update(*entityManager_, nowSeconds);
    audioSystem_->Update(*entityManager_);
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
    bool hasVisibleObjectContent = false;
    entityManager_->ForEach<UnlitGeometryRenderComponent>(
        [&](EntityID, UnlitGeometryRenderComponent& component) {
            hasVisibleObjectContent =
                hasVisibleObjectContent ||
                component.maskVisibility_.HasVisibleEntries();
        });
    {
        ScopedCpuTimer timer(
            performanceTimingStats_.get(),
            PerformanceSubsystem::EnvironmentDepth,
            true);
        environmentDepthSystem_->Update(
            *entityManager_, in, hasVisibleObjectContent);
    }
    {
        ScopedCpuTimer timer(
            performanceTimingStats_.get(),
            PerformanceSubsystem::LightEstimation,
            true);
        cameraLightEstimationSystem_->Update(*entityManager_, in, Focused);
    }
    unlitGeometryRenderSystem_->Update(*entityManager_, in);
#if defined(__ANDROID__)
    RefreshColorMatchingUi();
    RefreshMeshScaleUi();
#endif
    std::vector<OVRFW::TinyUI::HitTestDevice> pointerDevices;
    if ((ui_ || playbackUi_) && uiVisible_) {
        if (ui_) ui_->HitTestDevices().clear();
        if (playbackUi_) playbackUi_->HitTestDevices().clear();
        entityManager_->ForEach<InputComponent>([&](EntityID, InputComponent& input) {
            for (size_t handIndex = 0; handIndex < input.hands.size(); ++handIndex) {
                const HandInput& hand = input.hands[handIndex];
                const ControllerInput& controller = input.controllers[handIndex];
                if (hand.active && hand.aimValid) {
                    if (ui_) ui_->AddHitTestRay(
                        hand.aimPose, hand.indexPinching, static_cast<int>(handIndex));
                    if (playbackUi_) playbackUi_->AddHitTestRay(
                        hand.aimPose, hand.indexPinching, static_cast<int>(handIndex));
                } else if (controller.tracked) {
                    if (ui_) ui_->AddHitTestRay(
                        controller.aimPose, controller.indexTrigger > 0.5f,
                        static_cast<int>(handIndex));
                    if (playbackUi_) playbackUi_->AddHitTestRay(
                        controller.aimPose, controller.indexTrigger > 0.5f,
                        static_cast<int>(handIndex));
                }
            }
        });
        if (ui_) ui_->Update(in);
        if (playbackUi_) playbackUi_->Update(in);
        const auto mergePointerDevices =
            [&pointerDevices](const std::vector<OVRFW::TinyUI::HitTestDevice>& devices) {
                for (const auto& device : devices) {
                    auto existing = std::find_if(
                        pointerDevices.begin(),
                        pointerDevices.end(),
                        [&device](const OVRFW::TinyUI::HitTestDevice& candidate) {
                            return candidate.deviceNum == device.deviceNum;
                        });
                    if (existing == pointerDevices.end()) {
                        pointerDevices.push_back(device);
                        continue;
                    }
                    const float existingDistance =
                        (existing->pointerEnd - existing->pointerStart).LengthSq();
                    const float candidateDistance =
                        (device.pointerEnd - device.pointerStart).LengthSq();
                    if (device.hitObject &&
                        (!existing->hitObject || candidateDistance < existingDistance)) {
                        *existing = device;
                    }
                }
        };
        if (ui_) mergePointerDevices(ui_->HitTestDevices());
        if (playbackUi_) mergePointerDevices(playbackUi_->HitTestDevices());
#if defined(__ANDROID__)
        PreviewColorMatchingDraft();
#endif
        if (uiRebuildPending_) {
            const UiMode nextMode = pendingUiMode_;
            uiRebuildPending_ = false;
            if (nextMode == UiMode::Masks) BuildMaskSelector();
            else if (nextMode == UiMode::ColorMatching) BuildColorMatchingControls();
            else if (nextMode == UiMode::ColorMatchingSettings) BuildColorMatchingSettingsControls();
            else if (nextMode == UiMode::MeshScale) BuildMeshScaleControls();
            else if (nextMode == UiMode::MeshDetail) BuildMeshDetailControls();
            else if (nextMode == UiMode::Diagnostics) BuildDiagnosticsControls();
            else BuildDatasetPicker();
        }
    }
    if (pointerRenderer_) {
        pointerRenderer_->Update(in, pointerDevices);
    }
    const auto updateEnd = clock::now();
    performanceTimingStats_->EndForegroundFrame(
        std::chrono::duration<double, std::milli>(updateEnd - now).count());
    if (performanceTimingStats_->PublishIfReady(updateEnd)) {
        RefreshDiagnosticOverlay();
    }
}

void TDGenPlayerApp::AppRenderFrame(const OVRFW::ovrApplFrameIn& in, OVRFW::ovrRendererOutput& out)
{
    ScopedCpuTimer cpuTimer(
        performanceTimingStats_.get(),
        PerformanceSubsystem::Rendering);
    ScopedGpuTimer gpuTimer(
        gpuTiming_.get(),
        PerformanceSubsystem::Rendering);
    OVRFW::XrApp::AppRenderFrame(in, out);
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
    // TinyUI only appends draw surfaces. It must run before XrApp renders and
    // resolves the eye buffers, which happens inside the base AppRenderFrame().
    if (ui_ && uiVisible_) {
        ui_->Render(in, out);
    }
    if (playbackUi_ && uiVisible_) {
        playbackUi_->Render(in, out);
    }
    if (diagnosticUi_ && diagnosticOverlayVisible_) {
        diagnosticUi_->Render(in, out);
    }
    // Append the depth-tested pointer after the UI so it can compare against
    // panel depth and remain visible only for the ray segments in front.
    if (pointerRenderer_ && uiVisible_) {
        pointerRenderer_->Render(in, out);
    }
}

void TDGenPlayerApp::SessionEnd()
{
    StopHaptics();
    lastUpdateSeconds_ = 0.0;
    frameTimingStats_.Reset();
    xrFrameTimingStats_.Reset();
    supportedDisplayRefreshRates_.clear();
    requestedDisplayRefreshRate_ = 0.0f;
    currentPanelRefreshRate_ = 0.0f;
    currentPanelRefreshRateValid_ = false;
    displayRefreshRateMessage_.clear();
    performanceTimingStats_->Reset();
    if (gpuTiming_) gpuTiming_->Shutdown();
    uiAnchorInitialized_ = false;
    entityManager_->ForEach<UnlitGeometryRenderComponent>(
        [](EntityID, UnlitGeometryRenderComponent& render) {
            render.poseInitialized = false;
        });
    //xrInput_.Destroy();
    inputSystem_->SessionEnd(*entityManager_);
    cameraLightEstimationSystem_->SessionEnd(*entityManager_);
    environmentDepthSystem_->SessionEnd(*entityManager_);
    coreSystem_->SessionEnd(*entityManager_);
}

void TDGenPlayerApp::AppShutdown(const xrJava *context)
{
    if (pointerRenderer_) {
        pointerRenderer_->Shutdown();
        pointerRenderer_.reset();
    }
    ShutdownPlaybackControls();
    ShutdownDiagnosticOverlay();
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
    gpuTiming_.reset();

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
    refreshRateStatusLabel_ = nullptr;
    refreshRateMessageLabel_ = nullptr;
    meshScaleValueLabel_ = nullptr;
    meshScaleCurrentLabel_ = nullptr;
}

bool TDGenPlayerApp::PrepareUi() {
    uiStatusLabel_ = nullptr;
    refreshRateStatusLabel_ = nullptr;
    refreshRateMessageLabel_ = nullptr;
    meshScaleValueLabel_ = nullptr;
    meshScaleCurrentLabel_ = nullptr;
    if (ui_) {
        ui_->Clear();
    } else {
        ui_ = std::make_unique<OVRFW::TinyUI>();
        if (!ui_->Init(GetContext(), GetFileSys())) {
            LOGE("Failed to initialize main UI");
            ui_.reset();
            return false;
        }
    }
    ui_->SetPose(uiAnchorPose_);
    return true;
}

void TDGenPlayerApp::BuildPlaybackControls() {
#if defined(__ANDROID__)
    ShutdownPlaybackControls();
    playbackUi_ = std::make_unique<OVRFW::TinyUI>();
    if (!playbackUi_->Init(GetContext(), GetFileSys())) {
        LOGE("Failed to initialize playback controls UI");
        playbackUi_.reset();
        return;
    }
    playbackUi_->SetPose(uiAnchorPose_);
    // The bar keeps TinyUI's dark background. The button texture supplies its
    // own contrasting fill and border; these white multipliers preserve those
    // colors while still giving hover/click feedback.
    playbackUi_->BackgroundColor = {1.0f, 1.0f, 1.0f, 1.0f};
    playbackUi_->HoverColor = {0.72f, 0.82f, 1.0f, 1.0f};
    playbackUi_->HighlightColor = {1.0f, 0.88f, 0.62f, 1.0f};
    playbackUi_->AddLabel("", {0.0f, -0.48f, -1.52f}, {900.0f, 70.0f});
    playbackButton_ = playbackUi_->AddButton(
        "", {0.0f, -0.48f, -1.518f}, {80.0f, 80.0f},
        [this]() { TogglePlayback(); });
    if (playbackButton_) {
        playbackButton_->SetSurfaceBorder(0, OVR::Vector4f::ZERO);
        playbackButton_->SetSurfaceDims(0, {44.0f, 44.0f});
        playbackButton_->RegenerateSurfaceGeometry(0, false);
        playbackButton_->SetSurfaceColor(0, playbackUi_->BackgroundColor);
    }
    RefreshPlaybackControls();
#endif
}

void TDGenPlayerApp::ShutdownPlaybackControls() {
    if (playbackUi_) {
        playbackUi_->Shutdown();
        playbackUi_.reset();
    }
    playbackButton_ = nullptr;
}

void TDGenPlayerApp::BuildDiagnosticOverlay() {
#if defined(__ANDROID__)
    // TinyUI uses 500 texels per metre. The widest always-present menu is
    // playbackUi_ at 900 texels, centered at x=0, so its right edge is 0.90 m.
    // A 500-texel diagnostic panel is 1.00 m wide. Centering it at 1.48 m puts
    // its left edge at 0.98 m, leaving an exact 0.08 m horizontal gap.
    constexpr float diagnosticCenterX = 1.48f;
    constexpr float diagnosticWidthTexels = 520.0f;
    ShutdownDiagnosticOverlay();
    diagnosticUi_ = std::make_unique<OVRFW::TinyUI>();
    if (!diagnosticUi_->Init(GetContext(), GetFileSys())) {
        LOGE("Failed to initialize diagnostic overlay");
        diagnosticUi_.reset();
        return;
    }
    diagnosticUi_->SetPose(uiAnchorPose_);
    diagnosticLabel_ = diagnosticUi_->AddLabel(
        "Panel refresh rate: --.- Hz\n"
        "Application/XR FPS: --.-/--.-\n"
        "Playback: --  source: --.- FPS\n"
        "Missed frames: --\n"
        "Frame p50/p95/p99: --/--/-- ms\n"
        "Subsystem       CPU ms  GPU ms\n"
        "Env depth           --     N/A\n"
        "Camera capture      --     N/A\n"
        "Light estimate      --      --\n"
        "Depth resize        --     N/A\n"
        "Geometry compact    --     N/A\n"
        "Index upload        --     N/A\n"
        "Video decode        --     N/A\n"
        "Texture upload      --      --\n"
        "Rendering           --      --\n"
        "Other update        --     N/A",
        {diagnosticCenterX, 0.24f, -1.48f},
        {diagnosticWidthTexels, 800.0f});
    diagnosticCameraLabel_ = diagnosticUi_->AddLabel(
        "Decode avg/p95 ms\n"
        " Color codec/copy: --/--  --/--\n"
        " Alpha codec/copy: --/--  --/--\n"
        " Depth codec/copy: --/--  --/--\n"
        " Demux/audio: --/--\n"
        "Producer: -- fps  ring --/--  starve --\n"
        "Cells K/R/M: --/--/--  EBO: --\n"
        "Camera: --\n"
        "cb/proc/super/drop: --/--/--/--\n"
        "age/cb95/import95: --/--/-- ms copy: --",
        {diagnosticCenterX, -0.48f, -1.48f},
        {diagnosticWidthTexels, 520.0f});
    RefreshDiagnosticOverlay();
#endif
}

void TDGenPlayerApp::ShutdownDiagnosticOverlay() {
    if (diagnosticUi_) {
        diagnosticUi_->Shutdown();
        diagnosticUi_.reset();
    }
    diagnosticLabel_ = nullptr;
    diagnosticCameraLabel_ = nullptr;
}

void TDGenPlayerApp::RefreshDiagnosticOverlay() {
    if (!diagnosticLabel_ || !diagnosticCameraLabel_) return;
    const PerformanceTimingSnapshot snapshot =
        performanceTimingStats_->Snapshot();
    const bool gpuSupported = gpuTiming_ && gpuTiming_->IsSupported();
    const auto metricText = [&](PerformanceSubsystem subsystem,
                                PerformanceDomain domain,
                                bool applicable) {
        if (!applicable ||
            (domain == PerformanceDomain::Gpu && !gpuSupported)) {
            return std::string("N/A");
        }
        const auto& metric = snapshot.Get(subsystem, domain);
        if (!snapshot.valid || !metric.HasSamples()) return std::string("--");
        std::ostringstream value;
        value << std::fixed << std::setprecision(2)
              << metric.averageMilliseconds;
        return value.str();
    };
    const auto cpu = [&](PerformanceSubsystem subsystem) {
        return metricText(subsystem, PerformanceDomain::Cpu, true);
    };
    const auto cpuAverageP95 = [&](PerformanceSubsystem subsystem) {
        const auto& metric =
            snapshot.Get(subsystem, PerformanceDomain::Cpu);
        if (!snapshot.valid || !metric.HasSamples()) return std::string("--/--");
        std::ostringstream value;
        value << std::fixed << std::setprecision(2)
              << metric.averageMilliseconds << "/"
              << metric.p95Milliseconds;
        return value.str();
    };
    const auto gpu = [&](PerformanceSubsystem subsystem, bool applicable) {
        return metricText(subsystem, PerformanceDomain::Gpu, applicable);
    };
    CameraCaptureDiagnostics cameraDiagnostics{};
    uint32_t retainedCells = 0;
    uint32_t rejectedCells = 0;
    uint32_t mixedCells = 0;
    size_t compactIndexBytes = 0;
    bool usingDynamicIndices = false;
    bool playbackPaused = false;
    double sourceFps = 0.0;
    std::uint64_t producedFrames = 0;
    std::uint64_t ringStarvations = 0;
    int ringLowWater = 0;
    int ringOccupancy = 0;
    std::int64_t producerStartNanoseconds = 0;
    if (entityManager_) {
        entityManager_->ForEach<FrameLoaderComponent>(
            [&](EntityID, FrameLoaderComponent& loader) {
                playbackPaused =
                    loader.paused.load(std::memory_order_acquire);
                sourceFps = loader.fps;
            });
        entityManager_->ForEach<FrameLoaderState>(
            [&](EntityID, FrameLoaderState& state) {
                producedFrames =
                    state.producedFrameCount.load(std::memory_order_acquire);
                ringStarvations =
                    state.ringStarvationCount.load(std::memory_order_acquire);
                ringLowWater =
                    state.ringLowWaterMark.load(std::memory_order_acquire);
                producerStartNanoseconds =
                    state.producerStartNanoseconds.load(
                        std::memory_order_acquire);
                for (const FrameSlot& slot : state.ring) {
                    if (slot.ready.load(std::memory_order_acquire)) {
                        ++ringOccupancy;
                    }
                }
            });
        entityManager_->ForEach<CameraLightEstimationState>(
            [&](EntityID, CameraLightEstimationState& state) {
                cameraDiagnostics = state.captureDiagnostics;
            });
        entityManager_->ForEach<UnlitGeometryRenderState>(
            [&](EntityID, UnlitGeometryRenderState& state) {
                retainedCells = state.retainedCellCount_;
                rejectedCells = state.rejectedCellCount_;
                mixedCells = state.mixedCellCount_;
                compactIndexBytes = state.compactIndexBytes_;
                usingDynamicIndices = state.usingDynamicIndices_;
            });
    }
    const char* cameraPath =
        cameraDiagnostics.pipeline == CameraPipelineMode::RawExternalYuv
            ? "RawExternalYuv" :
        cameraDiagnostics.pipeline == CameraPipelineMode::CpuYuvPlanes
            ? "CpuYuvPlanes" : "Unavailable";
    const std::int64_t nowNanoseconds =
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    const double producerSeconds =
        producerStartNanoseconds > 0 && nowNanoseconds > producerStartNanoseconds
        ? static_cast<double>(nowNanoseconds - producerStartNanoseconds) /
            1000000000.0
        : 0.0;
    const double producerFps =
        producerSeconds > 0.0
        ? static_cast<double>(producedFrames) / producerSeconds
        : 0.0;

    std::ostringstream text;
    if (currentPanelRefreshRateValid_) {
        text << std::fixed << std::setprecision(1)
             << "Panel refresh rate: " << currentPanelRefreshRate_ << " Hz\n";
    } else {
        text << "Panel refresh rate: --.- Hz\n";
    }
    if (frameTimingStats_.HasPublishedSample() ||
        xrFrameTimingStats_.HasPublishedSample()) {
        text << std::fixed << std::setprecision(1)
             << "Application/XR FPS: ";
        if (frameTimingStats_.HasPublishedSample()) {
            text << frameTimingStats_.Fps();
        } else {
            text << "--.-";
        }
        text << "/";
        if (xrFrameTimingStats_.HasPublishedSample()) {
            text << xrFrameTimingStats_.Fps();
        } else {
            text << "--.-";
        }
        text << "\n";
    } else {
        text << "Application/XR FPS: --.-/--.-\n";
    }
    text << "Playback: " << (playbackPaused ? "paused" : "playing")
         << "  source: " << std::fixed << std::setprecision(1)
         << sourceFps << " FPS\n";
    if (frameTimingStats_.HasPanelRefreshRate()) {
        text << "Missed frames: "
             << frameTimingStats_.MissedFrameCount() << "\n";
    } else {
        text << "Missed frames: --\n";
    }
    if (frameTimingStats_.HasPublishedSample()) {
        text << std::fixed << std::setprecision(2)
             << "Frame p50/p95/p99: "
             << frameTimingStats_.P50FrameMilliseconds() << "/"
             << frameTimingStats_.P95FrameMilliseconds() << "/"
             << frameTimingStats_.P99FrameMilliseconds() << " ms\n";
    } else {
        text << "Frame p50/p95/p99: --/--/-- ms\n";
    }
    text << "Subsystem       CPU ms  GPU ms\n"
         << "Env depth       " << cpu(PerformanceSubsystem::EnvironmentDepth)
         << "  " << gpu(PerformanceSubsystem::EnvironmentDepth, false) << "\n"
         << "Camera capture  " << cpu(PerformanceSubsystem::CameraCapture)
         << "  " << gpu(PerformanceSubsystem::CameraCapture, false) << "\n"
         << "Light estimate  " << cpu(PerformanceSubsystem::LightEstimation)
         << "  " << gpu(PerformanceSubsystem::LightEstimation, true) << "\n"
         << "Depth resize    " << cpu(PerformanceSubsystem::DepthPreparation)
         << "  " << gpu(PerformanceSubsystem::DepthPreparation, false) << "\n"
         << "Geometry compact " << cpu(PerformanceSubsystem::GeometryCompaction)
         << "  " << gpu(PerformanceSubsystem::GeometryCompaction, false) << "\n"
         << "Index upload     " << cpu(PerformanceSubsystem::IndexUpload)
         << "  " << gpu(PerformanceSubsystem::IndexUpload, false) << "\n"
         << "Video decode    " << cpu(PerformanceSubsystem::VideoDecode)
         << "  " << gpu(PerformanceSubsystem::VideoDecode, false) << "\n"
         << "Texture upload  " << cpu(PerformanceSubsystem::TextureUpload)
         << "  " << gpu(PerformanceSubsystem::TextureUpload, true) << "\n"
         << "Rendering       " << cpu(PerformanceSubsystem::Rendering)
         << "  " << gpu(PerformanceSubsystem::Rendering, true) << "\n"
         << "Other update    " << cpu(PerformanceSubsystem::OtherUpdate)
         << "  " << gpu(PerformanceSubsystem::OtherUpdate, false);
    diagnosticLabel_->SetText(text.str().c_str());

    std::ostringstream cameraText;
    cameraText << "Decode avg/p95 ms\n"
         << " Color codec/copy: "
         << cpuAverageP95(PerformanceSubsystem::ColorDecode) << "  "
         << cpuAverageP95(PerformanceSubsystem::ColorCopy) << "\n"
         << " Alpha codec/copy: "
         << cpuAverageP95(PerformanceSubsystem::AlphaDecode) << "  "
         << cpuAverageP95(PerformanceSubsystem::AlphaCopy) << "\n"
         << " Depth codec/copy: "
         << cpuAverageP95(PerformanceSubsystem::DepthDecode) << "  "
         << cpuAverageP95(PerformanceSubsystem::DepthConvertCopy) << "\n"
         << " Demux/audio: "
         << cpuAverageP95(PerformanceSubsystem::DemuxAudio) << "\n"
         << "Producer: " << std::fixed << std::setprecision(1)
         << producerFps << " fps  ring "
         << ringOccupancy << "/" << ringLowWater
         << " cur/low  starve " << ringStarvations << "\n"
         << "Cells K/R/M: "
         << retainedCells << "/" << rejectedCells << "/" << mixedCells
         << "  EBO: "
         << (usingDynamicIndices
                ? std::to_string(compactIndexBytes / 1024) + " KiB"
                : std::string("full"))
         << "\nCamera: " << cameraPath << "\n"
         << "cb/proc/super/drop: "
         << cameraDiagnostics.callbackCount << "/"
         << cameraDiagnostics.processedCount << "/"
         << cameraDiagnostics.supersededFrameCount << "/"
         << cameraDiagnostics.queuePressureDrops << "\n"
         << std::fixed << std::setprecision(2)
         << "age/cb95/import95: " << cameraDiagnostics.latestFrameAgeMs << "/"
         << cameraDiagnostics.callbackP95Ms << "/"
         << cameraDiagnostics.importP95Ms << " ms copy: "
         << cameraDiagnostics.bytesCopied;
    diagnosticCameraLabel_->SetText(cameraText.str().c_str());
}

void TDGenPlayerApp::BuildDiagnosticsControls() {
    if (!PrepareUi()) return;
    currentUiMode_ = UiMode::Diagnostics;
    ui_->AddLabel("Diagnostics", {0.0f, 0.48f, -1.5f}, {620.0f, 60.0f});
    ui_->AddButton("Back", {0.0f, 0.39f, -1.5f}, {300.0f, 50.0f},
        [this]() { RequestUiMode(diagnosticsReturnMode_); });
    ui_->AddToggleButton(
        "Performance timings: On", "Performance timings: Off",
        &diagnosticOverlayVisible_, {0.0f, 0.30f, -1.5f}, {500.0f, 50.0f},
        [this]() { RefreshDiagnosticOverlay(); });
    ui_->AddToggleButton(
        "Dynamic index culling: On", "Dynamic index culling: Off",
        &dynamicIndexCullingEnabled_,
        {0.0f, 0.23f, -1.5f}, {500.0f, 50.0f},
        [this]() {
            if (!entityManager_) return;
            auto& loader =
                entityManager_->GetComponent<FrameLoaderComponent>(objectEntity_);
            loader.dynamicIndexCullingEnabled.store(
                dynamicIndexCullingEnabled_, std::memory_order_release);
        });

    std::ostringstream supported;
    supported << "Supported panel rates: ";
    if (!displayRefreshRateExtensionAvailable_) {
        supported << "unavailable";
    } else if (supportedDisplayRefreshRates_.empty()) {
        supported << "none";
    } else {
        supported << std::fixed << std::setprecision(1);
        for (size_t index = 0;
             index < supportedDisplayRefreshRates_.size();
             ++index) {
            if (index > 0) supported << ", ";
            supported << supportedDisplayRefreshRates_[index];
        }
        supported << " Hz";
    }
    ui_->AddLabel(supported.str(), {0.0f, 0.15f, -1.5f}, {720.0f, 45.0f});
    refreshRateStatusLabel_ = ui_->AddLabel(
        "", {0.0f, 0.08f, -1.5f}, {720.0f, 45.0f});

    const auto addRateChoice = [&](float refreshRate, float x) {
        std::ostringstream label;
        label << std::fixed << std::setprecision(0) << refreshRate << " Hz";
        if (SupportsDisplayRefreshRate(refreshRate)) {
            ui_->AddButton(
                label.str(),
                {x, -0.01f, -1.5f},
                {180.0f, 50.0f},
                [this, refreshRate]() {
                    if (RequestDisplayRefreshRate(refreshRate, true)) {
                        QueryCurrentPanelRefreshRate();
                    }
                    RefreshDisplayRefreshRateUi();
                    RefreshDiagnosticOverlay();
                });
        } else {
            label << " (unsupported)";
            ui_->AddLabel(
                label.str(), {x, -0.01f, -1.5f}, {240.0f, 50.0f});
        }
    };
    addRateChoice(72.0f, -0.27f);
    addRateChoice(90.0f, 0.27f);
    refreshRateMessageLabel_ = ui_->AddLabel(
        "", {0.0f, -0.08f, -1.5f}, {720.0f, 45.0f});
    RefreshDisplayRefreshRateUi();
}

void TDGenPlayerApp::OpenDiagnosticsControls(UiMode returnMode) {
    diagnosticsReturnMode_ = returnMode;
    RequestUiMode(UiMode::Diagnostics);
}

void TDGenPlayerApp::InitDisplayRefreshRateExtension() {
    displayRefreshRateExtensionAvailable_ = false;
#if defined(__ANDROID__)
    uint32_t extensionCount = 0;
    XrResult result = xrEnumerateInstanceExtensionProperties(
        nullptr, 0, &extensionCount, nullptr);
    if (XR_FAILED(result)) {
        LOGW(
            "Could not enumerate OpenXR extensions for display refresh rate (%d)",
            result);
        return;
    }
    std::vector<XrExtensionProperties> extensions(
        extensionCount, {XR_TYPE_EXTENSION_PROPERTIES});
    result = xrEnumerateInstanceExtensionProperties(
        nullptr,
        extensionCount,
        &extensionCount,
        extensions.data());
    if (XR_FAILED(result)) {
        LOGW(
            "Could not read OpenXR extensions for display refresh rate (%d)",
            result);
        return;
    }
    const bool extensionPresent = std::any_of(
        extensions.begin(),
        extensions.end(),
        [](const XrExtensionProperties& extension) {
            return std::strcmp(
                extension.extensionName,
                XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME) == 0;
        });
    if (!extensionPresent) {
        LOGW("XR_FB_display_refresh_rate is unavailable");
        return;
    }

    const XrResult enumerateResult = xrGetInstanceProcAddr(
        GetInstance(),
        "xrEnumerateDisplayRefreshRatesFB",
        reinterpret_cast<PFN_xrVoidFunction*>(
            &xrEnumerateDisplayRefreshRatesFB_));
    const XrResult getResult = xrGetInstanceProcAddr(
        GetInstance(),
        "xrGetDisplayRefreshRateFB",
        reinterpret_cast<PFN_xrVoidFunction*>(&xrGetDisplayRefreshRateFB_));
    const XrResult requestResult = xrGetInstanceProcAddr(
        GetInstance(),
        "xrRequestDisplayRefreshRateFB",
        reinterpret_cast<PFN_xrVoidFunction*>(&xrRequestDisplayRefreshRateFB_));
    displayRefreshRateExtensionAvailable_ =
        XR_SUCCEEDED(enumerateResult) &&
        XR_SUCCEEDED(getResult) &&
        XR_SUCCEEDED(requestResult) &&
        xrEnumerateDisplayRefreshRatesFB_ &&
        xrGetDisplayRefreshRateFB_ &&
        xrRequestDisplayRefreshRateFB_;
    if (displayRefreshRateExtensionAvailable_) {
        LOGI("XR_FB_display_refresh_rate initialized");
    } else {
        LOGW(
            "XR_FB_display_refresh_rate functions unavailable (%d/%d/%d)",
            enumerateResult,
            getResult,
            requestResult);
    }
#else
    displayRefreshRateMessage_ = "Panel refresh control is Android-only";
#endif
}

void TDGenPlayerApp::InitDisplayRefreshRateSession() {
    supportedDisplayRefreshRates_.clear();
    requestedDisplayRefreshRate_ = 0.0f;
    currentPanelRefreshRate_ = 0.0f;
    currentPanelRefreshRateValid_ = false;
    displayRefreshRateMessage_.clear();
    frameTimingStats_.SetPanelRefreshRate(0.0);
    xrFrameTimingStats_.SetPanelRefreshRate(0.0);

    if (!displayRefreshRateExtensionAvailable_) {
        displayRefreshRateMessage_ =
            "XR_FB_display_refresh_rate unavailable";
        return;
    }

    const bool enumerated = EnumerateDisplayRefreshRates();
    if (enumerated) {
        float preferred = ReadRequestedDisplayRefreshRate();
        if (preferred != 72.0f && preferred != 90.0f) {
            LOGW(
                "Ignoring invalid stored refresh rate %.3f; using 72.0 Hz",
                preferred);
            preferred = 72.0f;
        }

        float target = 0.0f;
        if (SupportsDisplayRefreshRate(preferred)) {
            target = preferred;
        } else if (SupportsDisplayRefreshRate(72.0f)) {
            LOGW(
                "Stored refresh rate %.1f Hz is unsupported; falling back to 72.0 Hz",
                preferred);
            target = 72.0f;
        }
        if (target > 0.0f) {
            RequestDisplayRefreshRate(target, true);
        } else {
            displayRefreshRateMessage_ =
                "Neither preferred rate nor 72.0 Hz is supported";
        }
    }

    QueryCurrentPanelRefreshRate();
    RefreshDisplayRefreshRateUi();
    RefreshDiagnosticOverlay();
}

bool TDGenPlayerApp::EnumerateDisplayRefreshRates() {
    supportedDisplayRefreshRates_.clear();
    if (!displayRefreshRateExtensionAvailable_ ||
        !xrEnumerateDisplayRefreshRatesFB_ ||
        GetSession() == XR_NULL_HANDLE) {
        return false;
    }

    uint32_t count = 0;
    XrResult result = xrEnumerateDisplayRefreshRatesFB_(
        GetSession(), 0, &count, nullptr);
    if (XR_FAILED(result)) {
        displayRefreshRateMessage_ =
            "Failed to enumerate supported panel rates";
        LOGW("xrEnumerateDisplayRefreshRatesFB(count) failed: %d", result);
        return false;
    }
    supportedDisplayRefreshRates_.resize(count);
    result = xrEnumerateDisplayRefreshRatesFB_(
        GetSession(),
        count,
        &count,
        supportedDisplayRefreshRates_.data());
    if (XR_FAILED(result)) {
        supportedDisplayRefreshRates_.clear();
        displayRefreshRateMessage_ =
            "Failed to enumerate supported panel rates";
        LOGW("xrEnumerateDisplayRefreshRatesFB(values) failed: %d", result);
        return false;
    }
    supportedDisplayRefreshRates_.resize(count);
    std::sort(
        supportedDisplayRefreshRates_.begin(),
        supportedDisplayRefreshRates_.end());
    std::ostringstream rates;
    rates << std::fixed << std::setprecision(1);
    for (size_t index = 0;
         index < supportedDisplayRefreshRates_.size();
         ++index) {
        if (index > 0) rates << ", ";
        rates << supportedDisplayRefreshRates_[index];
    }
    LOGI("Supported panel refresh rates: %s Hz", rates.str().c_str());
    return true;
}

bool TDGenPlayerApp::QueryCurrentPanelRefreshRate() {
    if (!displayRefreshRateExtensionAvailable_ ||
        !xrGetDisplayRefreshRateFB_ ||
        GetSession() == XR_NULL_HANDLE) {
        return false;
    }
    float refreshRate = 0.0f;
    const XrResult result =
        xrGetDisplayRefreshRateFB_(GetSession(), &refreshRate);
    if (XR_FAILED(result) ||
        !std::isfinite(refreshRate) ||
        refreshRate <= 0.0f) {
        currentPanelRefreshRate_ = 0.0f;
        currentPanelRefreshRateValid_ = false;
        frameTimingStats_.SetPanelRefreshRate(0.0);
        xrFrameTimingStats_.SetPanelRefreshRate(0.0);
        if (!displayRefreshRateMessage_.empty()) {
            displayRefreshRateMessage_ += "; ";
        }
        displayRefreshRateMessage_ +=
            "Failed to query current panel rate";
        LOGW(
            "xrGetDisplayRefreshRateFB failed or returned invalid rate: %d, %.3f",
            result,
            refreshRate);
        return false;
    }
    currentPanelRefreshRate_ = refreshRate;
    currentPanelRefreshRateValid_ = true;
    frameTimingStats_.SetPanelRefreshRate(refreshRate);
    xrFrameTimingStats_.SetPanelRefreshRate(refreshRate);
    LOGI("Current panel refresh rate: %.1f Hz", refreshRate);
    return true;
}

bool TDGenPlayerApp::RequestDisplayRefreshRate(
        float refreshRate,
        bool persist) {
    if (!displayRefreshRateExtensionAvailable_ ||
        !xrRequestDisplayRefreshRateFB_ ||
        GetSession() == XR_NULL_HANDLE) {
        displayRefreshRateMessage_ =
            "Panel refresh-rate requests are unavailable";
        return false;
    }
    if (!SupportsDisplayRefreshRate(refreshRate)) {
        std::ostringstream message;
        message << std::fixed << std::setprecision(1)
                << refreshRate << " Hz is not advertised";
        displayRefreshRateMessage_ = message.str();
        LOGW(
            "Not requesting unadvertised display refresh rate %.1f Hz",
            refreshRate);
        return false;
    }

    const XrResult result =
        xrRequestDisplayRefreshRateFB_(GetSession(), refreshRate);
    if (XR_FAILED(result)) {
        std::ostringstream message;
        message << std::fixed << std::setprecision(1)
                << "Request for " << refreshRate << " Hz failed ("
                << result << ")";
        displayRefreshRateMessage_ = message.str();
        LOGW(
            "xrRequestDisplayRefreshRateFB(%.1f) failed: %d",
            refreshRate,
            result);
        return false;
    }

    requestedDisplayRefreshRate_ = refreshRate;
    const bool saved = !persist || StoreRequestedDisplayRefreshRate(refreshRate);
    std::ostringstream message;
    message << std::fixed << std::setprecision(1)
            << "Requested " << refreshRate << " Hz";
    if (!saved) {
        message << "; preference save failed";
    }
    displayRefreshRateMessage_ = message.str();
    LOGI(
        "Requested display refresh rate %.1f Hz%s",
        refreshRate,
        saved ? "" : " (preference save failed)");
    return true;
}

bool TDGenPlayerApp::SupportsDisplayRefreshRate(float refreshRate) const {
    return std::find(
        supportedDisplayRefreshRates_.begin(),
        supportedDisplayRefreshRates_.end(),
        refreshRate) != supportedDisplayRefreshRates_.end();
}

void TDGenPlayerApp::RefreshDisplayRefreshRateUi() {
    if (refreshRateStatusLabel_) {
        std::ostringstream status;
        status << std::fixed << std::setprecision(1) << "Requested: ";
        if (requestedDisplayRefreshRate_ > 0.0f) {
            status << requestedDisplayRefreshRate_ << " Hz";
        } else {
            status << "--";
        }
        status << " | Actual panel: ";
        if (currentPanelRefreshRateValid_) {
            status << currentPanelRefreshRate_ << " Hz";
        } else {
            status << "--";
        }
        refreshRateStatusLabel_->SetText(status.str().c_str());
    }
    if (refreshRateMessageLabel_) {
        refreshRateMessageLabel_->SetText(
            displayRefreshRateMessage_.c_str());
    }
}

float TDGenPlayerApp::ReadRequestedDisplayRefreshRate() {
#if defined(__ANDROID__)
    const xrJava* java = GetContext();
    if (!java || !java->Env || !java->ActivityObject) {
        return 72.0f;
    }
    JNIEnv* env = java->Env;
    jclass activityClass = env->GetObjectClass(java->ActivityObject);
    jmethodID method = env->GetMethodID(
        activityClass, "loadRequestedDisplayRefreshRate", "()F");
    const float refreshRate = method
        ? env->CallFloatMethod(java->ActivityObject, method)
        : 72.0f;
    env->DeleteLocalRef(activityClass);
    if (env->ExceptionCheck()) {
        env->ExceptionDescribe();
        env->ExceptionClear();
        return 72.0f;
    }
    return refreshRate;
#else
    return 72.0f;
#endif
}

bool TDGenPlayerApp::StoreRequestedDisplayRefreshRate(float refreshRate) {
#if defined(__ANDROID__)
    if (refreshRate != 72.0f && refreshRate != 90.0f) {
        return false;
    }
    const xrJava* java = GetContext();
    if (!java || !java->Env || !java->ActivityObject) {
        return false;
    }
    JNIEnv* env = java->Env;
    jclass activityClass = env->GetObjectClass(java->ActivityObject);
    jmethodID method = env->GetMethodID(
        activityClass, "saveRequestedDisplayRefreshRate", "(F)Z");
    const bool saved = method &&
        env->CallBooleanMethod(
            java->ActivityObject,
            method,
            refreshRate) == JNI_TRUE;
    env->DeleteLocalRef(activityClass);
    if (env->ExceptionCheck()) {
        env->ExceptionDescribe();
        env->ExceptionClear();
        return false;
    }
    return saved;
#else
    (void)refreshRate;
    return false;
#endif
}

void TDGenPlayerApp::TogglePlayback() {
    if (!frameLoaderSystem_ || !entityManager_ || objectEntity_ == 0) return;
    auto& loader = entityManager_->GetComponent<FrameLoaderComponent>(objectEntity_);
    auto& state = entityManager_->GetComponent<FrameLoaderState>(objectEntity_);
    using clock = std::chrono::steady_clock;
    const double now = std::chrono::duration<double>(clock::now().time_since_epoch()).count();
    frameLoaderSystem_->TogglePaused(now, loader, state);
    RefreshPlaybackControls();
}

void TDGenPlayerApp::RefreshPlaybackControls() {
    if (!playbackButton_ || !entityManager_ || objectEntity_ == 0) return;
    const auto& loader = entityManager_->GetComponent<FrameLoaderComponent>(objectEntity_);
    const GLuint circularTexture = CreateCircularButtonTexture(
        loader.paused.load(std::memory_order_acquire));
    playbackButton_->SetSurfaceTextureTakeOwnership(
        0,
        0,
        OVRFW::SURFACE_TEXTURE_DIFFUSE_ALPHA_DISCARD,
        circularTexture,
        64,
        64);
    playbackButton_->SetSurfaceColor(0, playbackUi_->BackgroundColor);
}

void TDGenPlayerApp::BuildDatasetPicker() {
    if (!PrepareUi()) return;
    currentUiMode_ = UiMode::Datasets;
    uiStatusLabel_ = ui_->AddLabel("ViPE datasets", {0.0f, 0.42f, -1.5f}, {500.0f, 70.0f});
#if defined(__ANDROID__)
    ui_->AddButton("Color matching", {0.0f, 0.31f, -1.5f}, {500.0f, 60.0f},
        [this]() { OpenColorMatchingControls(UiMode::Datasets); });
    ui_->AddButton("Mesh scale", {0.0f, 0.22f, -1.5f}, {500.0f, 60.0f},
        [this]() { OpenMeshScaleControls(UiMode::Datasets); });
    ui_->AddButton("Mesh detail", {0.0f, 0.13f, -1.5f}, {500.0f, 60.0f},
        [this]() { OpenMeshDetailControls(UiMode::Datasets); });
    ui_->AddButton("Diagnostics", {0.0f, 0.04f, -1.5f}, {500.0f, 60.0f},
        [this]() { OpenDiagnosticsControls(UiMode::Datasets); });
#endif
    auto& loader = entityManager_->GetComponent<FrameLoaderComponent>(objectEntity_);
    float y =
#if defined(__ANDROID__)
        -0.07f;
#else
        0.11f;
#endif
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
    if (!PrepareUi()) return;
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
    ui_->AddButton("Mesh scale", {0.0f, 0.17f, -1.5f}, {620.0f, 60.0f},
        [this]() { OpenMeshScaleControls(UiMode::Masks); });
    ui_->AddButton("Mesh detail", {0.0f, 0.07f, -1.5f}, {620.0f, 60.0f},
        [this]() { OpenMeshDetailControls(UiMode::Masks); });
    ui_->AddButton("Diagnostics", {0.0f, -0.03f, -1.5f}, {620.0f, 60.0f},
        [this]() { OpenDiagnosticsControls(UiMode::Masks); });
#endif

    for (int id = 0; id < 256; ++id) {
        maskToggleValues_[static_cast<size_t>(id)] =
            render.maskVisibility_.IsVisible(static_cast<uint8_t>(id));
    }
    float y =
#if defined(__ANDROID__)
        -0.14f;
#else
        0.06f;
#endif
    for (const MaskVisibilityEntry& entry : render.maskVisibility_.Entries()) {
        const uint8_t id = entry.id;
        const std::string suffix = std::to_string(static_cast<unsigned int>(id)) +
            " - " + entry.label;
        ui_->AddToggleButton("Visible: " + suffix, "Hidden: " + suffix,
            &maskToggleValues_[id], {0.0f, y, -1.5f}, {620.0f, 60.0f},
            [this, id]() {
                auto& component = entityManager_->GetComponent<UnlitGeometryRenderComponent>(objectEntity_);
                component.maskVisibility_.SetVisible(id, maskToggleValues_[id]);
                auto& loader =
                    entityManager_->GetComponent<FrameLoaderComponent>(objectEntity_);
                loader.maskVisibilityPublisher->Publish(
                    component.maskVisibility_.ShaderValues());
            });
        y -= 0.10f;
    }
}

void TDGenPlayerApp::BuildMeshScaleControls() {
    if (!PrepareUi()) return;
    currentUiMode_ = UiMode::MeshScale;
    auto& transform = entityManager_->GetComponent<TransformComponent>(objectEntity_);
    auto& interactable = entityManager_->GetComponent<InteractableComponent>(objectEntity_);
    meshScaleLockUiValue_ = interactable.scaleLocked;
    meshScaleUiLockedSnapshot_ = interactable.scaleLocked;
    meshScaleUiValueSnapshot_ = transform.modelScale.x;

    const auto formatScale = [](float scale) {
        std::ostringstream stream;
        stream << std::fixed << std::setprecision(scale < 0.1f ? 4 : scale < 10.0f ? 3 : 2)
               << scale << "×";
        return stream.str();
    };
    const std::string value = formatScale(transform.modelScale.x);
    ui_->AddLabel("Mesh scale", {0.0f, 0.48f, -1.5f}, {620.0f, 60.0f});
    meshScaleCurrentLabel_ = ui_->AddLabel(
        "Current scale: " + value, {0.0f, 0.38f, -1.5f}, {620.0f, 55.0f});
    ui_->AddButton("Back", {0.0f, 0.28f, -1.5f}, {300.0f, 55.0f},
        [this]() { RequestUiMode(meshScaleReturnMode_); });
    ui_->AddToggleButton("Scale locked", "Scale unlocked", &meshScaleLockUiValue_,
        {0.0f, 0.18f, -1.5f}, {500.0f, 55.0f}, [this]() {
            auto& component = entityManager_->GetComponent<InteractableComponent>(objectEntity_);
            component.scaleLocked = meshScaleLockUiValue_;
            RequestUiMode(UiMode::MeshScale);
        });

    // At 500 texels/meter, the previous 150-texel value panel overlapped both
    // 60-texel step controls. Keep every surface coplanar, but leave a 2 cm
    // horizontal gap between the value and each button to avoid z-fighting.
    constexpr float scaleRowY = 0.06f;
    constexpr float decrementX = -0.18f;
    constexpr float valueX = 0.0f;
    constexpr float incrementX = 0.18f;
    const OVR::Vector2f stepSize = {50.0f, 50.0f};
    const OVR::Vector2f valueSize = {110.0f, 50.0f};
    ui_->AddLabel("Scale", {-0.36f, scaleRowY, -1.5f}, {100.0f, 50.0f});
    if (interactable.scaleLocked) {
        ui_->AddLabel("-", {decrementX, scaleRowY, -1.5f}, stepSize);
        meshScaleValueLabel_ =
            ui_->AddLabel(value, {valueX, scaleRowY, -1.5f}, valueSize);
        ui_->AddLabel("+", {incrementX, scaleRowY, -1.5f}, stepSize);
    } else {
        ui_->AddButton("-", {decrementX, scaleRowY, -1.5f}, stepSize,
            [this]() { StepMeshScale(-1); });
        meshScaleValueLabel_ =
            ui_->AddLabel(value, {valueX, scaleRowY, -1.5f}, valueSize);
        ui_->AddButton("+", {incrementX, scaleRowY, -1.5f}, stepSize,
            [this]() { StepMeshScale(1); });
    }
    ui_->AddButton("Reset to 1×", {0.0f, -0.07f, -1.5f}, {500.0f, 55.0f},
        [this]() { ResetMeshScale(); });
}

void TDGenPlayerApp::OpenMeshScaleControls(UiMode returnMode) {
    meshScaleReturnMode_ = returnMode;
    RequestUiMode(UiMode::MeshScale);
}

void TDGenPlayerApp::SetMeshScale(float scale) {
    auto& transform = entityManager_->GetComponent<TransformComponent>(objectEntity_);
    auto& transformState = entityManager_->GetComponent<TransformState>(objectEntity_);
    const auto& interactable = entityManager_->GetComponent<InteractableComponent>(objectEntity_);
    const float clamped = ScaleControl::Clamp(
        scale, interactable.minimumScale, interactable.maximumScale);
    interactionSystem_->CancelManipulation(*entityManager_);
    TransformSystem::SetScale(transform, transformState, {clamped, clamped, clamped});
    RefreshMeshScaleUi();
}

void TDGenPlayerApp::StepMeshScale(int direction) {
    const auto& interactable = entityManager_->GetComponent<InteractableComponent>(objectEntity_);
    const auto& transform = entityManager_->GetComponent<TransformComponent>(objectEntity_);
    const float scale = ScaleControl::StepLogarithmically(
        transform.modelScale.x, direction, interactable.scaleLocked,
        interactable.minimumScale, interactable.maximumScale);
    if (!interactable.scaleLocked) SetMeshScale(scale);
}

void TDGenPlayerApp::ResetMeshScale() {
    SetMeshScale(1.0f);
}

void TDGenPlayerApp::BuildMeshDetailControls() {
    if (!PrepareUi()) return;
    currentUiMode_ = UiMode::MeshDetail;
    const auto& loader =
        entityManager_->GetComponent<FrameLoaderComponent>(objectEntity_);
    const int divisor = MeshDetailControl::SanitizeDivisor(meshDetailDraft_.divisor);
    const int meshWidth =
        MeshDetailControl::ReducedDimension(loader.width, divisor);
    const int meshHeight =
        MeshDetailControl::ReducedDimension(loader.height, divisor);
    const std::size_t vertices =
        MeshDetailControl::VertexCount(loader.width, loader.height, divisor);

    ui_->AddLabel("Mesh detail", {0.0f, 0.48f, -1.5f}, {620.0f, 40.0f});
    ui_->AddLabel(
        "Video: " + std::to_string(loader.width) + " x " +
            std::to_string(loader.height),
        {0.0f, 0.38f, -1.5f},
        {620.0f, 40.0f});
    ui_->AddLabel(
        std::string("Saved: ") +
            MeshDetailControl::DisplayName(meshDetailSaved_.divisor) +
            " | Preview: " + MeshDetailControl::DisplayName(divisor),
        {0.0f, 0.28f, -1.5f},
        {620.0f, 40.0f});
    ui_->AddLabel(
        "Mesh: " + std::to_string(meshWidth) + " x " +
            std::to_string(meshHeight) + " | " +
            std::to_string(vertices) + " vertices",
        {0.0f, 0.18f, -1.5f},
        {620.0f, 40.0f});

    const auto addChoice = [&](int choice, float x, float y) {
        const std::string name = MeshDetailControl::DisplayName(choice);
        if (choice == divisor) {
            ui_->AddLabel(name, {x, y, -1.5f}, {80.0f, 50.0f});
        } else {
            ui_->AddButton(name, {x, y, -1.5f}, {80.0f, 50.0f},
                [this, choice]() { PreviewMeshDetail(choice); });
        }
    };
    addChoice(1, -0.27f, 0.06f);
    addChoice(2, -0.09f, 0.06f);
    addChoice(3, 0.09f, 0.06f);
    addChoice(4, 0.27f, 0.06f);
    ui_->AddButton("Save", {-0.24f, -0.07f, -1.5f}, {220.0f, 50.0f},
        [this]() { SaveMeshDetail(); });
    ui_->AddButton("Back", {0.24f, -0.07f, -1.5f}, {220.0f, 50.0f},
        [this]() {
            CancelMeshDetailEdits();
            RequestUiMode(meshDetailReturnMode_);
        });
    if (!meshDetailMessage_.empty()) {
        ui_->AddLabel(
            meshDetailMessage_, {0.0f, -0.18f, -1.5f}, {620.0f, 40.0f});
    }
}

void TDGenPlayerApp::OpenMeshDetailControls(UiMode returnMode) {
    meshDetailReturnMode_ = returnMode;
    meshDetailDraft_ = meshDetailSaved_;
    meshDetailMessage_.clear();
    meshDetailEditActive_ = true;
    RequestUiMode(UiMode::MeshDetail);
}

void TDGenPlayerApp::PreviewMeshDetail(int divisor) {
    if (!MeshDetailControl::IsValidDivisor(divisor)) {
        meshDetailMessage_ = "Invalid mesh detail level";
    } else if (!unlitGeometryRenderSystem_->RebuildGeometry(
                   *entityManager_, divisor)) {
        meshDetailMessage_ = "Preview failed; previous mesh kept";
    } else {
        meshDetailDraft_.divisor = divisor;
        meshDetailMessage_ = "Previewing " +
            std::string(MeshDetailControl::DisplayName(divisor));
    }
    RequestUiMode(UiMode::MeshDetail);
}

void TDGenPlayerApp::SaveMeshDetail() {
    const int divisor = meshDetailDraft_.divisor;
    if (!MeshDetailControl::IsValidDivisor(divisor)) {
        meshDetailMessage_ = "Save failed: invalid detail level";
    } else if (!StoreMeshDetailDivisor(divisor)) {
        meshDetailMessage_ = "Save failed: Android storage error";
    } else {
        meshDetailSaved_ = meshDetailDraft_;
        meshDetailMessage_ = "Saved globally";
    }
    RequestUiMode(UiMode::MeshDetail);
}

void TDGenPlayerApp::CancelMeshDetailEdits() {
    if (!meshDetailEditActive_) return;
    if (meshDetailDraft_.divisor != meshDetailSaved_.divisor &&
        !unlitGeometryRenderSystem_->RebuildGeometry(
            *entityManager_, meshDetailSaved_.divisor)) {
        LOGE("Failed to restore saved mesh detail");
    }
    meshDetailDraft_ = meshDetailSaved_;
    meshDetailEditActive_ = false;
    meshDetailMessage_.clear();
}

void TDGenPlayerApp::RefreshMeshScaleUi() {
    if (currentUiMode_ != UiMode::MeshScale || uiRebuildPending_) return;
    const auto& interactable = entityManager_->GetComponent<InteractableComponent>(objectEntity_);
    const auto& transform = entityManager_->GetComponent<TransformComponent>(objectEntity_);
    if (interactable.scaleLocked != meshScaleUiLockedSnapshot_) {
        RequestUiMode(UiMode::MeshScale);
        return;
    }
    if (transform.modelScale.x == meshScaleUiValueSnapshot_) return;
    meshScaleUiValueSnapshot_ = transform.modelScale.x;
    std::ostringstream stream;
    stream << std::fixed << std::setprecision(
        transform.modelScale.x < 0.1f ? 4 : transform.modelScale.x < 10.0f ? 3 : 2)
           << transform.modelScale.x << "×";
    const std::string value = stream.str();
    if (meshScaleCurrentLabel_) meshScaleCurrentLabel_->SetText(
        "Current scale: %s", value.c_str());
    if (meshScaleValueLabel_) meshScaleValueLabel_->SetText("%s", value.c_str());
}

void TDGenPlayerApp::BuildColorMatchingControls() {
    if (!PrepareUi()) return;
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
    const ColorMatchingTier selectedTier = colorMatchingEditActive_
        ? colorMatchingDraft_.requestedTier : component->requestedTier;
    colorMatchingUiRequested_ = selectedTier;
    colorMatchingUiActive_ = state->tier;
    colorMatchingUiGlobal_ = state->globalAvailability;
    colorMatchingUiSpatial_ = state->spatialAvailability;
    colorMatchingUiAvailabilityMessage_ = state->availabilityMessage;

    ui_->AddLabel("Color matching", {0.0f, 0.48f, -1.5f}, {620.0f, 60.0f});
    const TierAvailability selectedAvailability =
        selectedTier == ColorMatchingTier::Spatial ? state->spatialAvailability :
        selectedTier == ColorMatchingTier::Global ? state->globalAvailability :
        TierAvailability::Available;
    const char* active = state->tier == LightEstimateTier::Spatial ? "Spatial" :
        state->tier == LightEstimateTier::Global ? "Global" :
        selectedTier == ColorMatchingTier::Disabled ? "Disabled" :
        selectedAvailability == TierAvailability::Checking ? "Checking..." :
        selectedAvailability == TierAvailability::Available ? "Starting..." :
        "Unavailable";
    std::string status = std::string("Selected: ") +
        ColorMatchingTierName(selectedTier) + " | Active: " + active;
    if (colorMatchingEditActive_ && colorMatchingDraft_ != colorMatchingSaved_) {
        status += " | Unsaved";
    }
    uiStatusLabel_ = ui_->AddLabel(status, {0.0f, 0.38f, -1.5f}, {620.0f, 55.0f});
    ui_->AddButton("Back", {0.0f, 0.29f, -1.5f}, {300.0f, 50.0f},
        [this]() { CancelColorMatchingEdits(); RequestUiMode(colorMatchingReturnMode_); });
    ui_->AddButton("Edit settings", {0.0f, 0.21f, -1.5f}, {620.0f, 50.0f},
        [this]() { RequestUiMode(UiMode::ColorMatchingSettings); });
    ui_->AddButton("Save", {0.0f, 0.13f, -1.5f}, {300.0f, 50.0f},
        [this]() { SaveColorMatchingDraft(); });
    ui_->AddButton("Reset defaults", {0.0f, 0.05f, -1.5f}, {400.0f, 50.0f},
        [this]() { ResetColorMatchingDraft(); });
    if (!colorMatchingSettingsMessage_.empty()) {
        ui_->AddLabel(colorMatchingSettingsMessage_, {0.0f, -0.03f, -1.5f}, {620.0f, 45.0f});
    }

    const auto addTierRow = [&](ColorMatchingTier tier, TierAvailability availability, float y) {
        const std::string name = ColorMatchingTierName(tier);
        if (selectedTier == tier) {
            std::string label = name + " (Selected";
            if (!IsTierSelectable(
                    tier, state->globalAvailability, state->spatialAvailability)) {
                label += ", ";
                label += TierAvailabilityName(availability);
            }
            label += ")";
            ui_->AddLabel(label, {0.0f, y, -1.5f}, {620.0f, 60.0f});
        } else if (!IsTierSelectable(
                       tier, state->globalAvailability, state->spatialAvailability)) {
            ui_->AddLabel(name + " (" + TierAvailabilityName(availability) + ")",
                {0.0f, y, -1.5f}, {620.0f, 60.0f});
        } else {
            ui_->AddButton(name, {0.0f, y, -1.5f}, {620.0f, 60.0f},
                [this, tier]() { SelectColorMatchingTier(tier); });
        }
    };
    addTierRow(ColorMatchingTier::Disabled, TierAvailability::Available, -0.12f);
    addTierRow(ColorMatchingTier::Global, state->globalAvailability, -0.21f);
    addTierRow(ColorMatchingTier::Spatial, state->spatialAvailability, -0.30f);
    if (!state->availabilityMessage.empty()) {
        ui_->AddLabel(
            state->availabilityMessage, {0.0f, -0.39f, -1.5f}, {620.0f, 45.0f});
    }
}

void TDGenPlayerApp::BuildColorMatchingSettingsControls() {
    if (!PrepareUi()) return;
    currentUiMode_ = UiMode::ColorMatchingSettings;
    ui_->AddLabel("Color matching settings", {0.0f, 0.48f, -1.5f}, {620.0f, 55.0f});
    ui_->AddLabel("Dataset: " + colorMatchingSettingsDatasetId_,
        {0.0f, 0.40f, -1.5f}, {620.0f, 45.0f});
    ui_->AddButton("Back to color matching", {0.0f, 0.32f, -1.5f}, {620.0f, 48.0f},
        [this]() { RequestUiMode(UiMode::ColorMatching); });
    ui_->AddSlider("Strength", { -0.30f, 0.21f, -1.5f},
        &colorMatchingDraft_.matchingStrength, 1.0f, 0.05f, 0.0f, 1.0f);
    ui_->AddSlider("Smoothing", {-0.30f, 0.11f, -1.5f},
        &colorMatchingDraft_.temporalSmoothing, 0.85f, 0.05f, 0.0f, 0.95f);
    ui_->AddSlider("Min tint", {-0.30f, 0.01f, -1.5f},
        &colorMatchingDraft_.minTint, 0.7f, 0.05f, 0.25f, 1.0f);
    ui_->AddSlider("Max tint", {-0.30f, -0.09f, -1.5f},
        &colorMatchingDraft_.maxTint, 1.4f, 0.05f, 1.0f, 3.0f);
    ui_->AddSlider("Min exposure", {-0.30f, -0.19f, -1.5f},
        &colorMatchingDraft_.minExposure, 0.05f, 0.05f, 0.02f, 1.0f);
    ui_->AddSlider("Max exposure", {-0.30f, -0.29f, -1.5f},
        &colorMatchingDraft_.maxExposure, 2.0f, 0.05f, 1.0f, 4.0f);
}

void TDGenPlayerApp::OpenColorMatchingControls(UiMode returnMode) {
    colorMatchingReturnMode_ = returnMode;
    const auto& loader = entityManager_->GetComponent<FrameLoaderComponent>(objectEntity_);
    if (colorMatchingSettingsDatasetId_ != loader.selectedDatasetId) {
        LoadColorMatchingSettingsForDataset();
    }
    colorMatchingDraft_ = colorMatchingSaved_;
    colorMatchingPreviewed_ = colorMatchingDraft_;
    colorMatchingEditActive_ = true;
    colorMatchingSettingsMessage_.clear();
    RequestUiMode(UiMode::ColorMatching);
}

void TDGenPlayerApp::SelectColorMatchingTier(ColorMatchingTier tier) {
    entityManager_->ForEachMulti<CameraLightEstimationComponent, CameraLightEstimationState>(
        [&](EntityID, CameraLightEstimationComponent& component, CameraLightEstimationState& state) {
            if (!IsTierSelectable(tier, state.globalAvailability, state.spatialAvailability)) return;
            colorMatchingDraft_.requestedTier = tier;
            static_cast<ColorMatchingSettings&>(component) = colorMatchingDraft_;
            colorMatchingPreviewed_ = colorMatchingDraft_;
        });
    RequestUiMode(UiMode::ColorMatching);
}

void TDGenPlayerApp::RefreshColorMatchingUi() {
    if (currentUiMode_ != UiMode::ColorMatching || uiRebuildPending_) return;
    entityManager_->ForEachMulti<CameraLightEstimationComponent, CameraLightEstimationState>(
        [&](EntityID, CameraLightEstimationComponent& component, CameraLightEstimationState& state) {
            if (!colorMatchingUiSnapshotValid_ ||
                colorMatchingDraft_.requestedTier != colorMatchingUiRequested_ ||
                state.tier != colorMatchingUiActive_ ||
                state.globalAvailability != colorMatchingUiGlobal_ ||
                state.spatialAvailability != colorMatchingUiSpatial_ ||
                state.availabilityMessage != colorMatchingUiAvailabilityMessage_) {
                colorMatchingUiRequested_ = colorMatchingDraft_.requestedTier;
                colorMatchingUiActive_ = state.tier;
                colorMatchingUiGlobal_ = state.globalAvailability;
                colorMatchingUiSpatial_ = state.spatialAvailability;
                colorMatchingUiAvailabilityMessage_ = state.availabilityMessage;
                colorMatchingUiSnapshotValid_ = true;
                RequestUiMode(UiMode::ColorMatching);
            }
        });
}

void TDGenPlayerApp::PreviewColorMatchingDraft() {
    if (!colorMatchingEditActive_ ||
        (currentUiMode_ != UiMode::ColorMatching &&
         currentUiMode_ != UiMode::ColorMatchingSettings) ||
        colorMatchingDraft_ == colorMatchingPreviewed_) return;
    std::string error;
    if (!ValidateColorMatchingSettings(colorMatchingDraft_, error)) {
        colorMatchingSettingsMessage_ = "Invalid settings: " + error;
        return;
    }
    entityManager_->ForEach<CameraLightEstimationComponent>(
        [&](EntityID, CameraLightEstimationComponent& component) {
            static_cast<ColorMatchingSettings&>(component) = colorMatchingDraft_;
        });
    colorMatchingPreviewed_ = colorMatchingDraft_;
}

void TDGenPlayerApp::SaveColorMatchingDraft() {
    PreviewColorMatchingDraft();
    std::string error;
    if (!ValidateColorMatchingSettings(colorMatchingDraft_, error)) {
        colorMatchingSettingsMessage_ = "Save failed: " + error;
    } else if (colorMatchingSettingsDatasetId_.empty()) {
        colorMatchingSettingsMessage_ = "Save failed: no dataset selected";
    } else if (!StoreColorMatchingSettings(
                   colorMatchingSettingsDatasetId_, SerializeColorMatchingSettings(colorMatchingDraft_))) {
        colorMatchingSettingsMessage_ = "Save failed: Android storage error";
    } else {
        colorMatchingSaved_ = colorMatchingDraft_;
        colorMatchingSettingsMessage_ = "Saved for " + colorMatchingSettingsDatasetId_;
        LOGI("Saved color matching settings for dataset %s",
            colorMatchingSettingsDatasetId_.c_str());
    }
    RequestUiMode(UiMode::ColorMatching);
}

void TDGenPlayerApp::ResetColorMatchingDraft() {
    colorMatchingDraft_ = ColorMatchingSettings{};
    colorMatchingSettingsMessage_ = "Defaults previewed; select Save to keep them";
    PreviewColorMatchingDraft();
    RequestUiMode(UiMode::ColorMatching);
}

void TDGenPlayerApp::CancelColorMatchingEdits() {
    if (!colorMatchingEditActive_) return;
    if (colorMatchingDraft_ != colorMatchingSaved_) {
        entityManager_->ForEach<CameraLightEstimationComponent>(
            [&](EntityID, CameraLightEstimationComponent& component) {
                static_cast<ColorMatchingSettings&>(component) = colorMatchingSaved_;
            });
    }
    colorMatchingDraft_ = colorMatchingSaved_;
    colorMatchingPreviewed_ = colorMatchingSaved_;
    colorMatchingEditActive_ = false;
    colorMatchingSettingsMessage_.clear();
}

void TDGenPlayerApp::RequestUiMode(UiMode mode) {
    pendingUiMode_ = mode;
    uiRebuildPending_ = true;
}

void TDGenPlayerApp::LoadColorMatchingSettingsForDataset() {
#if defined(__ANDROID__)
    const auto& loader = entityManager_->GetComponent<FrameLoaderComponent>(objectEntity_);
    colorMatchingSettingsDatasetId_ = loader.selectedDatasetId;
    ColorMatchingSettings loaded;
    const std::string stored = colorMatchingSettingsDatasetId_.empty()
        ? std::string() : ReadColorMatchingSettings(colorMatchingSettingsDatasetId_);
    if (!stored.empty()) {
        std::string error;
        if (!ParseColorMatchingSettings(stored, loaded, error)) {
            LOGW("Ignoring invalid color matching settings for dataset %s: %s",
                colorMatchingSettingsDatasetId_.c_str(), error.c_str());
            loaded = ColorMatchingSettings{};
        } else {
            LOGI("Loaded color matching settings for dataset %s",
                colorMatchingSettingsDatasetId_.c_str());
        }
    }
    colorMatchingSaved_ = loaded;
    colorMatchingDraft_ = loaded;
    colorMatchingPreviewed_ = loaded;
    colorMatchingEditActive_ = false;
    colorMatchingSettingsMessage_.clear();
    entityManager_->ForEach<CameraLightEstimationComponent>(
        [&](EntityID, CameraLightEstimationComponent& component) {
            static_cast<ColorMatchingSettings&>(component) = loaded;
        });
#endif
}

std::string TDGenPlayerApp::ReadColorMatchingSettings(const std::string& datasetId) {
#if defined(__ANDROID__)
    const xrJava* java = GetContext();
    if (!java || !java->Env || !java->ActivityObject) return {};
    JNIEnv* env = java->Env;
    jclass activityClass = env->GetObjectClass(java->ActivityObject);
    jmethodID method = env->GetMethodID(
        activityClass, "loadColorMatchingSettings", "(Ljava/lang/String;)Ljava/lang/String;");
    jstring key = env->NewStringUTF(datasetId.c_str());
    jstring value = method ? static_cast<jstring>(
        env->CallObjectMethod(java->ActivityObject, method, key)) : nullptr;
    std::string result;
    if (value) {
        const char* text = env->GetStringUTFChars(value, nullptr);
        if (text) result = text;
        if (text) env->ReleaseStringUTFChars(value, text);
        env->DeleteLocalRef(value);
    }
    env->DeleteLocalRef(key);
    env->DeleteLocalRef(activityClass);
    if (env->ExceptionCheck()) {
        env->ExceptionDescribe();
        env->ExceptionClear();
        return {};
    }
    return result;
#else
    (void)datasetId;
    return {};
#endif
}

bool TDGenPlayerApp::StoreColorMatchingSettings(
        const std::string& datasetId, const std::string& json) {
#if defined(__ANDROID__)
    const xrJava* java = GetContext();
    if (!java || !java->Env || !java->ActivityObject) return false;
    JNIEnv* env = java->Env;
    jclass activityClass = env->GetObjectClass(java->ActivityObject);
    jmethodID method = env->GetMethodID(
        activityClass, "saveColorMatchingSettings", "(Ljava/lang/String;Ljava/lang/String;)Z");
    jstring key = env->NewStringUTF(datasetId.c_str());
    jstring value = env->NewStringUTF(json.c_str());
    const bool saved = method && env->CallBooleanMethod(
        java->ActivityObject, method, key, value) == JNI_TRUE;
    env->DeleteLocalRef(value);
    env->DeleteLocalRef(key);
    env->DeleteLocalRef(activityClass);
    if (env->ExceptionCheck()) {
        env->ExceptionDescribe();
        env->ExceptionClear();
        return false;
    }
    return saved;
#else
    (void)datasetId;
    (void)json;
    return false;
#endif
}

void TDGenPlayerApp::LoadMeshDetailSettings() {
#if defined(__ANDROID__)
    const int stored = ReadMeshDetailDivisor();
    meshDetailSaved_.divisor = MeshDetailControl::SanitizeDivisor(stored);
    meshDetailDraft_ = meshDetailSaved_;
    if (!MeshDetailControl::IsValidDivisor(stored)) {
        LOGW(
            "Invalid stored mesh detail divisor %d; using default %d",
            stored,
            meshDetailSaved_.divisor);
    } else {
        LOGI("Loaded global mesh detail divisor %d", meshDetailSaved_.divisor);
    }
#endif
}

int TDGenPlayerApp::ReadMeshDetailDivisor() {
#if defined(__ANDROID__)
    const xrJava* java = GetContext();
    if (!java || !java->Env || !java->ActivityObject) {
        return MeshDetailSettings{}.divisor;
    }
    JNIEnv* env = java->Env;
    jclass activityClass = env->GetObjectClass(java->ActivityObject);
    jmethodID method =
        env->GetMethodID(activityClass, "loadMeshDetailDivisor", "()I");
    const int divisor = method
        ? env->CallIntMethod(java->ActivityObject, method)
        : MeshDetailSettings{}.divisor;
    env->DeleteLocalRef(activityClass);
    if (env->ExceptionCheck()) {
        env->ExceptionDescribe();
        env->ExceptionClear();
        return MeshDetailSettings{}.divisor;
    }
    return divisor;
#else
    return MeshDetailSettings{}.divisor;
#endif
}

bool TDGenPlayerApp::StoreMeshDetailDivisor(int divisor) {
#if defined(__ANDROID__)
    if (!MeshDetailControl::IsValidDivisor(divisor)) return false;
    const xrJava* java = GetContext();
    if (!java || !java->Env || !java->ActivityObject) return false;
    JNIEnv* env = java->Env;
    jclass activityClass = env->GetObjectClass(java->ActivityObject);
    jmethodID method =
        env->GetMethodID(activityClass, "saveMeshDetailDivisor", "(I)Z");
    const bool saved = method &&
        env->CallBooleanMethod(java->ActivityObject, method, divisor) == JNI_TRUE;
    env->DeleteLocalRef(activityClass);
    if (env->ExceptionCheck()) {
        env->ExceptionDescribe();
        env->ExceptionClear();
        return false;
    }
    return saved;
#else
    (void)divisor;
    return false;
#endif
}

void TDGenPlayerApp::SelectDataset(const std::string& datasetId) {
    CancelColorMatchingEdits();
    auto& loader = entityManager_->GetComponent<FrameLoaderComponent>(objectEntity_);
    auto& loaderState = entityManager_->GetComponent<FrameLoaderState>(objectEntity_);
    unlitGeometryRenderSystem_->Shutdown(*entityManager_);
    const bool selected = frameLoaderSystem_->SelectDataset(datasetId, loader, loaderState);
    if (selected) {
        using clock = std::chrono::steady_clock;
        const double now = std::chrono::duration<double>(clock::now().time_since_epoch()).count();
        frameLoaderSystem_->SetPaused(false, now, loader, loaderState);
        RefreshPlaybackControls();
#if defined(__ANDROID__)
        LoadColorMatchingSettingsForDataset();
#endif
        auto& render = entityManager_->GetComponent<UnlitGeometryRenderComponent>(objectEntity_);
        render.maskVisibility_.Reset(loader.dataset.maskLabels);
        loader.maskVisibilityPublisher->Publish(
            render.maskVisibility_.ShaderValues());
        loader.dynamicIndexCullingEnabled.store(
            dynamicIndexCullingEnabled_, std::memory_order_release);
        unlitGeometryRenderSystem_->Init(*entityManager_, meshDetailSaved_.divisor);
        RequestUiMode(UiMode::Masks);
    } else {
        unlitGeometryRenderSystem_->Init(*entityManager_, meshDetailSaved_.divisor);
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
