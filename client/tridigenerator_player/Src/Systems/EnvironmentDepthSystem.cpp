#include <cmath>
#include <vector>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES3/gl3ext.h>
#include <jni.h>

#define XR_USE_GRAPHICS_API_OPENGL_ES 1
#define XR_USE_PLATFORM_ANDROID 1

#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <meta_openxr_preview/openxr_oculus_helpers.h>

#define LOG_TAG "EnvironmentDepthSystem"
#include "../Core/Logging.h"

#include "EnvironmentDepthSystem.h"

namespace {

XrResult CheckErrors(XrInstance instance, XrResult result, const char* function, bool failOnError) {
    if (XR_FAILED(result)) {
        char errorBuffer[XR_MAX_RESULT_STRING_SIZE];
        xrResultToString(instance, result, errorBuffer);
        if (failOnError) {
            LOGE("OpenXR error: %s: %s\n", function, errorBuffer);
        } else {
            LOGV("OpenXR error: %s: %s\n", function, errorBuffer);
        }
    }
    return result;
}

#if defined(DEBUG)
#define OXR(func) CheckErrors(instance_, func, #func, true);
#else
#define OXR(func) CheckErrors(instance_, func, #func, false);
#endif

inline OVR::Matrix4f OvrFromXr(const XrMatrix4x4f& x) {
    // Match SampleXrFramework conversion (column-major to row-major).
    return OVR::Matrix4f(
        x.m[0],
        x.m[4],
        x.m[8],
        x.m[12],
        x.m[1],
        x.m[5],
        x.m[9],
        x.m[13],
        x.m[2],
        x.m[6],
        x.m[10],
        x.m[14],
        x.m[3],
        x.m[7],
        x.m[11],
        x.m[15]);
}

} // namespace

EnvironmentDepthSystem::EnvironmentDepthSystem(XrInstance instance) : instance_{instance} {
}

bool EnvironmentDepthSystem::Init(EntityManager& ecs) {
    ecs.ForEach<EnvironmentDepthState>([](EntityID, EnvironmentDepthState& edS) {
        edS.Image.views[0].type = XR_TYPE_ENVIRONMENT_DEPTH_IMAGE_VIEW_META;
        edS.Image.views[1].type = XR_TYPE_ENVIRONMENT_DEPTH_IMAGE_VIEW_META;
    });
    return true;
}

void EnvironmentDepthSystem::Shutdown(EntityManager& ecs) {
    ecs.ForEachMulti<CoreState, EnvironmentDepthState>(
        [&](EntityID, CoreState& cS, EnvironmentDepthState& edS) {
            DestroyDepthResources(cS, edS);
        });
}

void EnvironmentDepthSystem::SessionInit(EntityManager& ecs, XrSession session) {
    ecs.ForEachMulti<CoreComponent, CoreState, EnvironmentDepthState>(
        [&](EntityID, CoreComponent& cC, CoreState& cS, EnvironmentDepthState& edS) {
            if (!cC.supportsDepth || session == XR_NULL_HANDLE) {
                return;
            }
            if (edS.IsInitialized) {
                return;
            }
            if (cS.XrCreateEnvironmentDepthProviderMETA == nullptr ||
                cS.XrCreateEnvironmentDepthSwapchainMETA == nullptr ||
                cS.XrEnumerateEnvironmentDepthSwapchainImagesMETA == nullptr ||
                cS.XrGetEnvironmentDepthSwapchainStateMETA == nullptr ||
                cS.XrStartEnvironmentDepthProviderMETA == nullptr) {
                LOGE("Depth: missing required function pointers");
                return;
            }

            const XrEnvironmentDepthProviderCreateInfoMETA providerCreateInfo{
                XR_TYPE_ENVIRONMENT_DEPTH_PROVIDER_CREATE_INFO_META};
            XrResult result = OXR(cS.XrCreateEnvironmentDepthProviderMETA(
                session, &providerCreateInfo, &cS.EnvironmentDepthProvider));
            if (XR_FAILED(result)) {
                DestroyDepthResources(cS, edS);
                return;
            }

            XrEnvironmentDepthSwapchainCreateInfoMETA swapchainCreateInfo{
                XR_TYPE_ENVIRONMENT_DEPTH_SWAPCHAIN_CREATE_INFO_META};
            result = OXR(cS.XrCreateEnvironmentDepthSwapchainMETA(
                cS.EnvironmentDepthProvider, &swapchainCreateInfo, &cS.EnvironmentDepthSwapchain));
            if (XR_FAILED(result)) {
                DestroyDepthResources(cS, edS);
                return;
            }

            XrEnvironmentDepthSwapchainStateMETA swapchainState{
                XR_TYPE_ENVIRONMENT_DEPTH_SWAPCHAIN_STATE_META};
            result = OXR(cS.XrGetEnvironmentDepthSwapchainStateMETA(
                cS.EnvironmentDepthSwapchain, &swapchainState));
            if (XR_FAILED(result)) {
                DestroyDepthResources(cS, edS);
                return;
            }
            edS.Width = swapchainState.width;
            edS.Height = swapchainState.height;

            uint32_t swapchainLength = 0;
            result = OXR(cS.XrEnumerateEnvironmentDepthSwapchainImagesMETA(
                cS.EnvironmentDepthSwapchain, 0, &swapchainLength, nullptr));
            if (XR_FAILED(result) || swapchainLength == 0) {
                DestroyDepthResources(cS, edS);
                return;
            }
            edS.SwapchainLength = swapchainLength;

            using SwapchainImageType = XrSwapchainImageOpenGLESKHR;
            static constexpr auto kSwapChainImageType = XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR;
            std::vector<SwapchainImageType> swapchainImages(swapchainLength);
            for (uint32_t i = 0; i < swapchainLength; ++i) {
                swapchainImages[i] = {kSwapChainImageType};
            }

            result = OXR(cS.XrEnumerateEnvironmentDepthSwapchainImagesMETA(
                cS.EnvironmentDepthSwapchain,
                swapchainLength,
                &swapchainLength,
                reinterpret_cast<XrSwapchainImageBaseHeader*>(swapchainImages.data())));
            if (XR_FAILED(result)) {
                DestroyDepthResources(cS, edS);
                return;
            }

            edS.SwapchainTextures.clear();
            edS.SwapchainTextures.reserve(swapchainLength);
            for (uint32_t i = 0; i < swapchainLength; ++i) {
                edS.SwapchainTextures.emplace_back(
                    static_cast<unsigned>(swapchainImages[i].image),
                    GL_TEXTURE_2D_ARRAY,
                    static_cast<int>(edS.Width),
                    static_cast<int>(edS.Height));
                OVRFW::MakeTextureClamped(edS.SwapchainTextures.back());
                OVRFW::MakeTextureNearest(edS.SwapchainTextures.back());
            }

            result = OXR(cS.XrStartEnvironmentDepthProviderMETA(cS.EnvironmentDepthProvider));
            if (XR_FAILED(result)) {
                DestroyDepthResources(cS, edS);
                return;
            }
            edS.IsInitialized = true;

            LOGI("Depth: provider+swapchain created (%ux%u, len=%u)",
                 edS.Width,
                 edS.Height,
                 edS.SwapchainLength);
        });
}

void EnvironmentDepthSystem::SessionEnd(EntityManager& ecs) {
    ecs.ForEachMulti<CoreState, EnvironmentDepthState>(
        [&](EntityID, CoreState& cS, EnvironmentDepthState& edS) {
            DestroyDepthResources(cS, edS);
        });
}

void EnvironmentDepthSystem::Update(EntityManager& ecs, const OVRFW::ovrApplFrameIn& in) {
    ecs.ForEachMulti<CoreComponent, CoreState, EnvironmentDepthState>(
        [&](EntityID, CoreComponent& cC, CoreState& cS, EnvironmentDepthState& edS) {
            if (!cC.supportsDepth || !edS.IsInitialized || cS.EnvironmentDepthProvider == XR_NULL_HANDLE) {
                edS.HasDepth = false;
                return;
            }
            if (cS.XrAcquireEnvironmentDepthImageMETA == nullptr) {
                edS.HasDepth = false;
                return;
            }

            edS.AcquireInfo.space = cS.localSpace;
            edS.AcquireInfo.displayTime = ToXrTime(in.PredictedDisplayTime);
            edS.Image.views[0].type = XR_TYPE_ENVIRONMENT_DEPTH_IMAGE_VIEW_META;
            edS.Image.views[1].type = XR_TYPE_ENVIRONMENT_DEPTH_IMAGE_VIEW_META;

            const XrResult acquireResult =
                cS.XrAcquireEnvironmentDepthImageMETA(cS.EnvironmentDepthProvider, &edS.AcquireInfo, &edS.Image);
            if (acquireResult != XR_SUCCESS) {
                edS.HasDepth = false;
                return;
            }

            edS.HasDepth = true;
            edS.NearZ = edS.Image.nearZ;
            edS.FarZ = edS.Image.farZ;

            for (int eye = 0; eye < EnvironmentDepthState::kNumEyes; ++eye) {
                const XrPosef xfLocalFromDepthEye = edS.Image.views[eye].pose;
                XrPosef xfDepthEyeFromLocal;
                XrPosef_Invert(&xfDepthEyeFromLocal, &xfLocalFromDepthEye);

                XrMatrix4x4f viewMat;
                XrMatrix4x4f_CreateFromRigidTransform(&viewMat, &xfDepthEyeFromLocal);

                XrMatrix4x4f projectionMat;
                XrMatrix4x4f_CreateProjectionFov(
                    &projectionMat,
                    GRAPHICS_OPENGL_ES,
                    edS.Image.views[eye].fov,
                    edS.Image.nearZ,
                    std::isfinite(edS.Image.farZ) ? edS.Image.farZ : 0);

                edS.DepthViewMatrices[eye] = OvrFromXr(viewMat);
                edS.DepthProjectionMatrices[eye] = OvrFromXr(projectionMat);
            }
        });
}

void EnvironmentDepthSystem::DestroyDepthResources(CoreState& cS, EnvironmentDepthState& edS) {
    edS.HasDepth = false;
    edS.IsInitialized = false;
    edS.SwapchainTextures.clear();
    edS.SwapchainLength = 0;
    edS.Width = 0;
    edS.Height = 0;

    if (cS.EnvironmentDepthProvider != XR_NULL_HANDLE && cS.XrStopEnvironmentDepthProviderMETA != nullptr) {
        OXR(cS.XrStopEnvironmentDepthProviderMETA(cS.EnvironmentDepthProvider));
    }
    if (cS.EnvironmentDepthSwapchain != XR_NULL_HANDLE && cS.XrDestroyEnvironmentDepthSwapchainMETA != nullptr) {
        OXR(cS.XrDestroyEnvironmentDepthSwapchainMETA(cS.EnvironmentDepthSwapchain));
    }
    if (cS.EnvironmentDepthProvider != XR_NULL_HANDLE && cS.XrDestroyEnvironmentDepthProviderMETA != nullptr) {
        OXR(cS.XrDestroyEnvironmentDepthProviderMETA(cS.EnvironmentDepthProvider));
    }

    cS.EnvironmentDepthSwapchain = XR_NULL_HANDLE;
    cS.EnvironmentDepthProvider = XR_NULL_HANDLE;
}
