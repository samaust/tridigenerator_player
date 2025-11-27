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

#include "TDGenPlayerApp.h"
#include "Logging.h"

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

TDGenPlayerApp::TDGenPlayerApp() :
        frameloader(std::make_unique<FrameLoader>("http://192.168.111.250:8080"))
{
    BackgroundColor = OVR::Vector4f(0.55f, 0.35f, 0.1f, 1.0f);

    // Disable framework input management, letting this sample explicitly
    // call xrSyncActions() every frame; which includes control over which
    // ActionSet to set as active
    SkipInputHandling = false;
}

TDGenPlayerApp::~TDGenPlayerApp()
{
}

// Returns a list of OpenXR extensions requested for this app
// Note that the sample framework will filter out any extension
// that is not listed as supported.
std::vector<const char *> TDGenPlayerApp::GetExtensions()
{
    std::vector<const char *> extensions = XrApp::GetExtensions();
    extensions.push_back(XR_EXT_HAND_TRACKING_EXTENSION_NAME);
    extensions.push_back(XR_FB_TOUCH_CONTROLLER_PRO_EXTENSION_NAME);
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

    // Load frame data
    if (!frameloader->LoadManifest()) {
        LOGE("Failed to load manifest");
        return false;
    }
    frameloader->StartBackgroundWriter();

    // Create initial plane geometry and renderer
    auto planeDescriptor = OVRFW::BuildTesselatedQuadDescriptor(
            frameloader->GetWidth()-1,
            frameloader->GetHeight()-1,
            true,
            false);
    OVR::Vector4f planeColor = {1.0f, 0.0f, 0.0f, 1.0f};
    planeGeometry_.Add(
            planeDescriptor,
            OVRFW::GeometryBuilder::kInvalidIndex,
            planeColor);

    auto d = planeGeometry_.ToGeometryDescriptor();
    //d.attribs.position = frame.positions;
    //d.attribs.color = frame.colors;

    planeRenderer_.Init(d);
    planeRenderer_.SetPose(
        OVR::Posef(OVR::Quat<float>::Identity(), {0_m, 1.5_m, -3.0_m}));
    //planeRenderer_.SetScale({1.0f, 1.0f, 1.0f});

    // Init texture for video frames
    planeRenderer_.CreateTexture(
        frameloader->GetWidth(),
        frameloader->GetHeight());

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

    return true;
}

// The update function is called every frame before the Render() function.
// Some of the key OpenXR function called by the framework prior to calling this function:
//  - xrPollEvent(...)
//  - xrWaitFrame(...)
void TDGenPlayerApp::Update(const OVRFW::ovrApplFrameIn &in)
{
    // if SkipInputHandling is True, we need to sync action sets ourselves
    // --- xrSyncAction
    //
    // Sync action sets (including default set if not skipped by SkipInputHandling)
    //OXR(xrSyncActions(Session, &syncInfo));

    // Application logic update here

    // Update plane geometry with latest frame data
    using clock = std::chrono::steady_clock;
    double now = std::chrono::duration<double>(clock::now().time_since_epoch()).count();

    // Try to advance playback if needed. Very fast: few atomics + a move when successful.
    bool consumed = frameloader->ReadFrameIfNeeded(now);

    if (consumed) {
        // get the most recent frame and use it for rendering
        const FrameData& frame = frameloader->GetCurrentFrame();
        LOGI("TDGenPlayerApp::Update frame.width=%d height=%d", frame.width, frame.height);
        if (frame.width > 0 && frame.height > 0) {
            //LOGI("New frame: %dx%d pts=%f", frame.width, frame.height, frame.pts);
            // Update textures
            LOGI("planeRenderer_.UpdateTexture()");
            planeRenderer_.UpdateTexture(
                reinterpret_cast<const uint8_t*>(frame.textureYData),
                reinterpret_cast<const uint8_t*>(frame.textureUData),
                reinterpret_cast<const uint8_t*>(frame.textureVData),
                static_cast<uint32_t>(frame.width),
                static_cast<uint32_t>(frame.height));
        }

        //auto d = planeGeometry_.ToGeometryDescriptor(); // TODO : Calculate once and store to improve performance
        //d.attribs.position = frame.positions;
        //d.attribs.color = frame.colors;
        //planeRenderer_.UpdateGeometry(d);
    }
    planeRenderer_.Update();
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
    planeRenderer_.Render(out.Surfaces);
}

void TDGenPlayerApp::SessionEnd()
{
    //xrInput_.Destroy();
}

void TDGenPlayerApp::AppShutdown(const xrJava *context)
{
    frameloader->StopBackgroundWriter();
    OVRFW::XrApp::AppShutdown(context);
}
