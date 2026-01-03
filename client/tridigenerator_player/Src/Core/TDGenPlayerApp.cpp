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
#include "../Components/TransformComponent.h"
#include "../Components/FrameLoaderComponent.h"
#include "../Components/RenderComponent.h"
#include "../Components/UnlitGeometryRenderComponent.h"

#include "../States/TransformState.h"
#include "../States/FrameLoaderState.h"
#include "../States/UnlitGeometryRenderState.h"

#include "../Systems/CoreSystem.h"
#include "../Systems/SceneSystem.h"
#include "../Systems/FrameLoaderSystem.h"
#include "../Systems/AudioSystem.h"
#include "../Systems/InputSystem.h"
#include "../Systems/RenderSystem.h"
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

    // Add hand tracking and controller extensions
    extensions.push_back(XR_EXT_HAND_TRACKING_EXTENSION_NAME);
    extensions.push_back(XR_FB_TOUCH_CONTROLLER_PRO_EXTENSION_NAME);

    // Add extensions from coreSystem_
    std::vector<const char*> coreSystemExtensions = coreSystem_->GetExtensions();
    extensions.insert(extensions.end(), coreSystemExtensions.begin(), coreSystemExtensions.end());

    return extensions;
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
    transformSystem_ = std::make_unique<TransformSystem>();
    renderSystem_ = std::make_unique<RenderSystem>();
    unlitGeometryRenderSystem_ = std::make_unique<UnlitGeometryRenderSystem>();
    LOGI("ECS Systems Initialized");

    // Create entities

    // ---------- Create Core entity ----------
    auto CoreEntity = entityManager_->CreateEntity();
    entityManager_->AddComponent<CoreComponent>(CoreEntity, {});
    entityManager_->AddComponent<CoreState>(CoreEntity, {});

    // ---------- Create Object entity ----------
    auto ObjectEntity = entityManager_->CreateEntity();
    TransformComponent transform;
    transform.modelPose = OVR::Posef(OVR::Quatf::Identity(), {0.0f, 0.0f, 0.0f});
    transform.modelScale = {1.0f, 1.0f, 1.0f};
    entityManager_->AddComponent<TransformComponent>(ObjectEntity, transform);
    entityManager_->AddComponent<TransformState>(ObjectEntity, {});
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
    transformSystem_->Init(*entityManager_);
    renderSystem_->Init(*entityManager_);
    unlitGeometryRenderSystem_->Init(*entityManager_);

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
    coreSystem_->SessionInit(*entityManager_, session);

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
    inputSystem_->Update(*entityManager_);
    transformSystem_->Update(*entityManager_);
    renderSystem_->Update(*entityManager_);
    unlitGeometryRenderSystem_->Update(*entityManager_, in);
}

void TDGenPlayerApp::AppRenderFrame(const OVRFW::ovrApplFrameIn& in, OVRFW::ovrRendererOutput& out)
{
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
}

void TDGenPlayerApp::SessionEnd()
{
    //xrInput_.Destroy();
    coreSystem_->SessionEnd(*entityManager_);
}

void TDGenPlayerApp::AppShutdown(const xrJava *context)
{
    // Explicitly destroy the systems and entity manager.
    // This is good practice to control the shutdown order.
    unlitGeometryRenderSystem_->Shutdown(*entityManager_);
    renderSystem_->Shutdown(*entityManager_);
    transformSystem_->Shutdown(*entityManager_);
    inputSystem_->Shutdown(*entityManager_);
    audioSystem_->Shutdown(*entityManager_);
    frameLoaderSystem_->Shutdown(*entityManager_);
    sceneSystem_->Shutdown(*entityManager_);
    coreSystem_->Shutdown(*entityManager_);

    unlitGeometryRenderSystem_.reset();
    renderSystem_.reset();
    transformSystem_.reset();
    inputSystem_.reset();
    audioSystem_.reset();
    frameLoaderSystem_.reset();
    sceneSystem_.reset();
    coreSystem_.reset();

    entityManager_.reset(); // Calls delete and empties the unique_ptr.
    LOGI("ECS Systems Shutdown");

    curl_global_cleanup();

    OVRFW::XrApp::AppShutdown(context);
}
// Insert passthrough layer before projection layers when available
void TDGenPlayerApp::PreProjectionAddLayer(xrCompositorLayerUnion* layers, int& layerCount) {
    XrCompositionLayerPassthroughFB passthroughLayer{XR_TYPE_COMPOSITION_LAYER_PASSTHROUGH_FB};
    if (coreSystem_->BuildPassthroughLayer(*entityManager_, passthroughLayer, XR_NULL_HANDLE)) {
        layers[layerCount++].Passthrough = passthroughLayer;
    }
}
