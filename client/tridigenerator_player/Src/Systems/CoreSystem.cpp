#include <iostream>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES3/gl3ext.h>
#include <jni.h>

#define XR_USE_GRAPHICS_API_OPENGL_ES 1
#define XR_USE_PLATFORM_ANDROID 1

#define LOG_TAG "CoreSystem"
#include "../Core/Logging.h"

#include "CoreSystem.h"

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

CoreSystem::CoreSystem(XrInstance instance, XrSystemId systemId, XrSpace localSpace)
    : instance_{instance}, systemId_{systemId}, localSpace_{localSpace} {
}

bool CoreSystem::Init(EntityManager& ecs) {
    ecs.ForEachMulti<CoreComponent, CoreState>(
            [&](EntityID e,
                CoreComponent &cC,
                CoreState &cS) {
        cS.localSpace = localSpace_;
        InitHandtracking(cC, cS);
        InitPassthrough(cC, cS);

    });
    return true;
}

std::vector<const char*> CoreSystem::GetExtensions() {
    std::vector<const char*> extensions = PassthroughRequiredExtensionNames();
    std::vector<const char*> depthExtensions = DepthRequiredExtensionNames();
    extensions.insert(extensions.end(), depthExtensions.begin(), depthExtensions.end());
    return extensions;
}

void CoreSystem::Shutdown(EntityManager& ecs) {
}

void CoreSystem::Update(EntityManager& ecs) {
}

void CoreSystem::InitHandtracking(CoreComponent& cC, CoreState& cS) {
    // Even if the runtime supports the hand tracking extension,
    // the actual device might not support hand tracking.
    // Inspect the system properties to find out.
    XrSystemHandTrackingPropertiesEXT handTrackingSystemProperties{
            XR_TYPE_SYSTEM_HAND_TRACKING_PROPERTIES_EXT};
    XrSystemProperties systemProperties{
            XR_TYPE_SYSTEM_PROPERTIES, &handTrackingSystemProperties};
    OXR(xrGetSystemProperties(instance_, systemId_, &systemProperties));
    cC.supportsHandTracking = handTrackingSystemProperties.supportsHandTracking;

    if (cC.supportsHandTracking) {
        /// Hook up extensions for hand tracking
        OXR(xrGetInstanceProcAddr(
                instance_,
                "xrCreateHandTrackerEXT",
                (PFN_xrVoidFunction*)(&cS.xrCreateHandTrackerEXT_)));
        assert(cS.xrCreateHandTrackerEXT_);

        OXR(xrGetInstanceProcAddr(
                instance_,
                "xrDestroyHandTrackerEXT",
                (PFN_xrVoidFunction*)(&cS.xrDestroyHandTrackerEXT_)));
        assert(cS.xrDestroyHandTrackerEXT_);

        OXR(xrGetInstanceProcAddr(
                instance_,
                "xrLocateHandJointsEXT",
                (PFN_xrVoidFunction*)(&cS.xrLocateHandJointsEXT_)));
        assert(cS.xrLocateHandJointsEXT_);
    }
}

std::vector<const char*> CoreSystem::PassthroughRequiredExtensionNames() {
    return {XR_FB_PASSTHROUGH_EXTENSION_NAME, XR_FB_TRIANGLE_MESH_EXTENSION_NAME};
}

std::vector<const char*> CoreSystem::DepthRequiredExtensionNames() {
    return {XR_META_ENVIRONMENT_DEPTH_EXTENSION_NAME};
}

void CoreSystem::InitPassthrough(CoreComponent& cC, CoreState& cS) {
    cC.supportsPassthrough = ExtensionsArePresent(PassthroughRequiredExtensionNames());
    cC.supportsDepth = ExtensionsArePresent(DepthRequiredExtensionNames());

    // Passthrough
    if (cC.supportsPassthrough) {
        LOGI("Passthrough: Required extensions present; initializing passthrough");

        // Hook up extensions for passthrough
        // XR_FB_passthrough
        OXR(xrGetInstanceProcAddr(
                instance_,
                "xrCreatePassthroughFB",
                reinterpret_cast<PFN_xrVoidFunction*>(&cS.XrCreatePassthroughFB)));
        assert(cS.XrCreatePassthroughFB);

        OXR(xrGetInstanceProcAddr(
                instance_,
                "xrDestroyPassthroughFB",
                reinterpret_cast<PFN_xrVoidFunction*>(&cS.XrDestroyPassthroughFB)));
        assert(cS.XrDestroyPassthroughFB);

        OXR(xrGetInstanceProcAddr(
                instance_,
                "xrPassthroughStartFB",
                reinterpret_cast<PFN_xrVoidFunction*>(&cS.XrPassthroughStartFB)));
        assert(cS.XrPassthroughStartFB);

        OXR(xrGetInstanceProcAddr(
                instance_,
                "xrPassthroughPauseFB",
                reinterpret_cast<PFN_xrVoidFunction*>(&cS.XrPassthroughPauseFB)));
        assert(cS.XrPassthroughPauseFB);

        OXR(xrGetInstanceProcAddr(
                instance_,
                "xrCreatePassthroughLayerFB",
                reinterpret_cast<PFN_xrVoidFunction*>(&cS.XrCreatePassthroughLayerFB)));
        assert(cS.XrCreatePassthroughLayerFB);

        OXR(xrGetInstanceProcAddr(
                instance_,
                "xrDestroyPassthroughLayerFB",
                reinterpret_cast<PFN_xrVoidFunction*>(&cS.XrDestroyPassthroughLayerFB)));
        assert(cS.XrDestroyPassthroughLayerFB);

        OXR(xrGetInstanceProcAddr(
                instance_,
                "xrPassthroughLayerSetStyleFB",
                reinterpret_cast<PFN_xrVoidFunction*>(&cS.XrPassthroughLayerSetStyleFB)));
        assert(cS.XrPassthroughLayerSetStyleFB);

        OXR(xrGetInstanceProcAddr(
                instance_,
                "xrCreateGeometryInstanceFB",
                reinterpret_cast<PFN_xrVoidFunction*>(&cS.XrCreateGeometryInstanceFB)));
        assert(cS.XrCreateGeometryInstanceFB);

        OXR(xrGetInstanceProcAddr(
                instance_,
                "xrDestroyGeometryInstanceFB",
                reinterpret_cast<PFN_xrVoidFunction*>(&cS.XrDestroyGeometryInstanceFB)));
        assert(cS.XrDestroyGeometryInstanceFB);

        OXR(xrGetInstanceProcAddr(
                instance_,
                "xrGeometryInstanceSetTransformFB",
                reinterpret_cast<PFN_xrVoidFunction*>(&cS.XrGeometryInstanceSetTransformFB)));
        assert(cS.XrGeometryInstanceSetTransformFB);

        // XR_FB_triangle_mesh
        OXR(xrGetInstanceProcAddr(
                instance_,
                "xrCreateTriangleMeshFB",
                reinterpret_cast<PFN_xrVoidFunction*>(&cS.XrCreateTriangleMeshFB)));
        assert(cS.XrCreateTriangleMeshFB);

        OXR(xrGetInstanceProcAddr(
                instance_,
                "xrDestroyTriangleMeshFB",
                reinterpret_cast<PFN_xrVoidFunction*>(&cS.XrDestroyTriangleMeshFB)));
        assert(cS.XrDestroyTriangleMeshFB);
    } else {
        LOGW("Passthrough: Required extensions not present; passthrough disabled");
    }

    // Environment Depth
    if (cC.supportsDepth) {
        LOGI("Depth: Required extensions present; initializing depth");

        // Hook up extensions for depth
        OXR(xrGetInstanceProcAddr(
                instance_,
                "xrCreateEnvironmentDepthProviderMETA",
                reinterpret_cast<PFN_xrVoidFunction*>(&cS.XrCreateEnvironmentDepthProviderMETA)));
        assert(cS.XrCreateEnvironmentDepthProviderMETA);

        OXR(xrGetInstanceProcAddr(
                instance_,
                "xrDestroyEnvironmentDepthProviderMETA",
                reinterpret_cast<PFN_xrVoidFunction*>(&cS.XrDestroyEnvironmentDepthProviderMETA)));
        assert(cS.XrDestroyEnvironmentDepthProviderMETA);

        OXR(xrGetInstanceProcAddr(
                instance_,
                "xrStartEnvironmentDepthProviderMETA",
                reinterpret_cast<PFN_xrVoidFunction*>(&cS.XrStartEnvironmentDepthProviderMETA)));
        assert(cS.XrStartEnvironmentDepthProviderMETA);

        OXR(xrGetInstanceProcAddr(
                instance_,
                "xrStopEnvironmentDepthProviderMETA",
                reinterpret_cast<PFN_xrVoidFunction*>(&cS.XrStopEnvironmentDepthProviderMETA)));
        assert(cS.XrStopEnvironmentDepthProviderMETA);

        OXR(xrGetInstanceProcAddr(
                instance_,
                "xrCreateEnvironmentDepthSwapchainMETA",
                reinterpret_cast<PFN_xrVoidFunction*>(&cS.XrCreateEnvironmentDepthSwapchainMETA)));
        assert(cS.XrCreateEnvironmentDepthSwapchainMETA);

        OXR(xrGetInstanceProcAddr(
                instance_,
                "xrDestroyEnvironmentDepthSwapchainMETA",
                reinterpret_cast<PFN_xrVoidFunction*>(&cS.XrDestroyEnvironmentDepthSwapchainMETA)));
        assert(cS.XrDestroyEnvironmentDepthSwapchainMETA);

        OXR(xrGetInstanceProcAddr(
                instance_,
                "xrEnumerateEnvironmentDepthSwapchainImagesMETA",
                reinterpret_cast<PFN_xrVoidFunction*>(&cS.XrEnumerateEnvironmentDepthSwapchainImagesMETA)));
        assert(cS.XrEnumerateEnvironmentDepthSwapchainImagesMETA);

        OXR(xrGetInstanceProcAddr(
                instance_,
                "xrGetEnvironmentDepthSwapchainStateMETA",
                reinterpret_cast<PFN_xrVoidFunction*>(&cS.XrGetEnvironmentDepthSwapchainStateMETA)));
        assert(cS.XrGetEnvironmentDepthSwapchainStateMETA);

        OXR(xrGetInstanceProcAddr(
                instance_,
                "xrAcquireEnvironmentDepthImageMETA",
                reinterpret_cast<PFN_xrVoidFunction*>(&cS.XrAcquireEnvironmentDepthImageMETA)));
        assert(cS.XrAcquireEnvironmentDepthImageMETA);

        OXR(xrGetInstanceProcAddr(
                instance_,
                "xrSetEnvironmentDepthHandRemovalMETA",
                reinterpret_cast<PFN_xrVoidFunction*>(&cS.XrSetEnvironmentDepthHandRemovalMETA)));
        assert(cS.XrSetEnvironmentDepthHandRemovalMETA);
    } else {
        LOGW("Depth: Required extensions not present; depth disabled");
    }
}

bool CoreSystem::ExtensionsArePresent(const std::vector<const char*>& extensionList) const {
    const auto extensionProperties = GetXrExtensionProperties();
    bool foundAllExtensions = true;
    for (const auto& extension : extensionList) {
        bool foundExtension = false;
        for (const auto& extensionProperty : extensionProperties) {
            if (!strcmp(extension, extensionProperty.extensionName)) {
                foundExtension = true;
                break;
            }
        }
        if (!foundExtension) {
            foundAllExtensions = false;
            break;
        }
    }
    return foundAllExtensions;
}

std::vector<XrExtensionProperties> CoreSystem::GetXrExtensionProperties() const {
    XrResult result;
    PFN_xrEnumerateInstanceExtensionProperties xrEnumerateInstanceExtensionProperties;
    OXR(result = xrGetInstanceProcAddr(
            XR_NULL_HANDLE,
            "xrEnumerateInstanceExtensionProperties",
            (PFN_xrVoidFunction*)&xrEnumerateInstanceExtensionProperties));
    if (result != XR_SUCCESS) {
        LOGE("Failed to get xrEnumerateInstanceExtensionProperties function pointer.");
        exit(1);
    }

    uint32_t numInputExtensions = 0;
    uint32_t numOutputExtensions = 0;
    OXR(xrEnumerateInstanceExtensionProperties(
            nullptr, numInputExtensions, &numOutputExtensions, nullptr));
    LOGV("xrEnumerateInstanceExtensionProperties found %u extension(s).", numOutputExtensions);

    numInputExtensions = numOutputExtensions;

    std::vector<XrExtensionProperties> extensionProperties(
            numOutputExtensions, {XR_TYPE_EXTENSION_PROPERTIES});

    OXR(xrEnumerateInstanceExtensionProperties(
            nullptr, numInputExtensions, &numOutputExtensions, extensionProperties.data()));
    for (uint32_t i = 0; i < numOutputExtensions; i++) {
        LOGV("Extension #%d = '%s'.", i, extensionProperties[i].extensionName);
    }

    return extensionProperties;
}

void CoreSystem::SessionInit(EntityManager& ecs, XrSession session) {
    ecs.ForEachMulti<CoreComponent, CoreState>(
            [&](EntityID e,
                CoreComponent &cC,
                CoreState &cS) {
        cS.Session = session;

        // Passthrough
        if (cC.supportsPassthrough && session != XR_NULL_HANDLE) {
            LOGI("Passthrough: Initializing for this session");
            XrPassthroughCreateInfoFB passthroughInfo{XR_TYPE_PASSTHROUGH_CREATE_INFO_FB};
            OXR(cS.XrCreatePassthroughFB(session, &passthroughInfo, &cS.Passthrough));

            // Create a projected layer and only start passthrough if layer creation succeeded
            XrPassthroughLayerFB createdLayer = CreateProjectedLayer(cS);
            if (createdLayer != XR_NULL_HANDLE) {
                cS.PassthroughLayer = createdLayer;
                PassthroughStart(cS);
            }
        } else {
            LOGW("Passthrough: Not initialized for this session");
        }
    });
}

void CoreSystem::SessionEnd(EntityManager& ecs) {
    ecs.ForEach<CoreState>(
            [&](EntityID e,
                CoreState &cS) {
        if (cS.Passthrough != XR_NULL_HANDLE) {
            if (cS.PassthroughLayer != XR_NULL_HANDLE) {
                DestroyLayer(cS);
                cS.PassthroughLayer = XR_NULL_HANDLE;
            }
            OXR(cS.XrDestroyPassthroughFB(cS.Passthrough));
        }
        cS.Session = XR_NULL_HANDLE;
    });
}

bool CoreSystem::BuildPassthroughLayer(
        EntityManager& ecs,
        XrCompositionLayerPassthroughFB& outLayer,
        XrSpace space) {
    bool builtLayer = false;
    ecs.ForEach<CoreState>(
            [&](EntityID e,
                CoreState &cS) {
        if (builtLayer) {
            return;
        }
        if (cS.PassthroughLayer != XR_NULL_HANDLE) {
            outLayer = {XR_TYPE_COMPOSITION_LAYER_PASSTHROUGH_FB};
            outLayer.layerHandle = cS.PassthroughLayer;
            outLayer.flags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
            outLayer.space = space;
            builtLayer = true;
        }
    });
    if (!builtLayer) {
        LOGW("BuildPassthroughLayer: No passthrough layer to add");
    }
    return builtLayer;
}

XrPassthroughLayerFB CoreSystem::CreateProjectedLayer(CoreState& cS) const {
    XrPassthroughLayerFB layer = XR_NULL_HANDLE;

    XrPassthroughLayerCreateInfoFB layerInfo{XR_TYPE_PASSTHROUGH_LAYER_CREATE_INFO_FB};
    layerInfo.passthrough = cS.Passthrough;
    layerInfo.purpose = XR_PASSTHROUGH_LAYER_PURPOSE_RECONSTRUCTION_FB;
    //layerInfo.purpose = XR_PASSTHROUGH_LAYER_PURPOSE_PROJECTED_FB; // TODO : improve XRPassthroughHelper to make this a parameter
    layerInfo.flags = XR_PASSTHROUGH_IS_RUNNING_AT_CREATION_BIT_FB;
    XrResult result = cS.XrCreatePassthroughLayerFB(cS.Session, &layerInfo, &layer);
    if (result != XR_SUCCESS) {
        LOGE("xrCreatePassthroughLayerFB failed, error %d", result);
        return XR_NULL_HANDLE;
    }

    XrPassthroughStyleFB style{XR_TYPE_PASSTHROUGH_STYLE_FB};
    style.textureOpacityFactor = 1.0f;
    style.edgeColor = {0.0f, 0.0f, 0.0f, 0.0f};
    result = cS.XrPassthroughLayerSetStyleFB(layer, &style);
    if (result != XR_SUCCESS) {
        LOGE("xrPassthroughLayerSetStyleFBfailed, error %d", result);
        return XR_NULL_HANDLE;
    }

    return layer;
}

void CoreSystem::DestroyLayer(CoreState& cS) const {
    OXR(cS.XrDestroyPassthroughLayerFB(cS.PassthroughLayer));
}

void CoreSystem::PassthroughStart(CoreState& cS) const {
    // Passthrough
    OXR(cS.XrPassthroughStartFB(cS.Passthrough));
}

void CoreSystem::PassthroughPause(CoreState& cS) const {
    OXR(cS.XrPassthroughPauseFB(cS.Passthrough));
}

// CreateGeometryInstance not implemented
// DestroyGeometryInstance not implemented
// SetGeometryInstanceTransform not implemented
// Add if we decide to use projected passthrough
