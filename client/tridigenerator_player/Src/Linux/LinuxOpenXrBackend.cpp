#include "LinuxOpenXrBackend.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include <GL/gl.h>

LinuxOpenXrBackend::~LinuxOpenXrBackend() {
    Shutdown();
}

std::string LinuxOpenXrBackend::ResultString(XrResult result) const {
    char text[XR_MAX_RESULT_STRING_SIZE] = {};
    if (instance_ != XR_NULL_HANDLE) xrResultToString(instance_, result, text);
    return text[0] ? text : std::to_string(result);
}

bool LinuxOpenXrBackend::Initialize(
    Display* display,
    GLXFBConfig framebufferConfig,
    GLXDrawable drawable,
    GLXContext context,
    std::string& error) {
    uint32_t extensionCount = 0;
    xrEnumerateInstanceExtensionProperties(nullptr, 0, &extensionCount, nullptr);
    std::vector<XrExtensionProperties> extensions(
        extensionCount, XrExtensionProperties{XR_TYPE_EXTENSION_PROPERTIES});
    xrEnumerateInstanceExtensionProperties(nullptr, extensionCount, &extensionCount, extensions.data());
    const auto openGlExtension = std::find_if(
        extensions.begin(), extensions.end(), [](const XrExtensionProperties& property) {
            return std::strcmp(property.extensionName, XR_KHR_OPENGL_ENABLE_EXTENSION_NAME) == 0;
        });
    if (openGlExtension == extensions.end()) {
        error = "OpenXR runtime does not support XR_KHR_opengl_enable";
        return false;
    }
    const char* enabledExtensions[] = {XR_KHR_OPENGL_ENABLE_EXTENSION_NAME};
    XrInstanceCreateInfo instanceInfo{XR_TYPE_INSTANCE_CREATE_INFO};
    std::snprintf(instanceInfo.applicationInfo.applicationName, XR_MAX_APPLICATION_NAME_SIZE,
        "%s", "TriDiGenerator Player");
    instanceInfo.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;
    instanceInfo.enabledExtensionCount = 1;
    instanceInfo.enabledExtensionNames = enabledExtensions;
    XrResult result = xrCreateInstance(&instanceInfo, &instance_);
    if (XR_FAILED(result)) {
        error = "No usable OpenXR runtime: xrCreateInstance failed (" + ResultString(result) +
            "). Use --backend desktop or configure a runtime.";
        return false;
    }
    XrInstanceProperties instanceProperties{XR_TYPE_INSTANCE_PROPERTIES};
    if (XR_SUCCEEDED(xrGetInstanceProperties(instance_, &instanceProperties))) {
        std::fprintf(stderr, "OpenXR runtime: %s\n", instanceProperties.runtimeName);
    }
    XrSystemGetInfo systemInfo{XR_TYPE_SYSTEM_GET_INFO};
    systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    result = xrGetSystem(instance_, &systemInfo, &systemId_);
    if (XR_FAILED(result)) {
        error = "OpenXR runtime has no available HMD system (" + ResultString(result) + ")";
        return false;
    }
    uint32_t configurationCount = 0;
    xrEnumerateViewConfigurations(instance_, systemId_, 0, &configurationCount, nullptr);
    std::vector<XrViewConfigurationType> configurations(configurationCount);
    xrEnumerateViewConfigurations(
        instance_, systemId_, configurationCount, &configurationCount, configurations.data());
    if (std::find(configurations.begin(), configurations.end(),
            XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO) == configurations.end()) {
        error = "The selected OpenXR runtime does not advertise PRIMARY_STEREO";
        return false;
    }
    PFN_xrGetOpenGLGraphicsRequirementsKHR getRequirements = nullptr;
    result = xrGetInstanceProcAddr(instance_, "xrGetOpenGLGraphicsRequirementsKHR",
        reinterpret_cast<PFN_xrVoidFunction*>(&getRequirements));
    if (XR_FAILED(result) || !getRequirements) {
        error = "Could not query OpenXR OpenGL graphics requirements";
        return false;
    }
    XrGraphicsRequirementsOpenGLKHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_KHR};
    result = getRequirements(instance_, systemId_, &requirements);
    if (XR_FAILED(result)) {
        error = "OpenXR rejected OpenGL requirements query: " + ResultString(result);
        return false;
    }
    XVisualInfo* visual = glXGetVisualFromFBConfig(display, framebufferConfig);
    XrGraphicsBindingOpenGLXlibKHR binding{XR_TYPE_GRAPHICS_BINDING_OPENGL_XLIB_KHR};
    binding.xDisplay = display;
    binding.visualid = visual ? visual->visualid : 0;
    binding.glxFBConfig = framebufferConfig;
    binding.glxDrawable = drawable;
    binding.glxContext = context;
    if (visual) XFree(visual);
    XrSessionCreateInfo sessionInfo{XR_TYPE_SESSION_CREATE_INFO};
    sessionInfo.next = &binding;
    sessionInfo.systemId = systemId_;
    result = xrCreateSession(instance_, &sessionInfo, &session_);
    if (XR_FAILED(result)) {
        error = "xrCreateSession failed: " + ResultString(result);
        return false;
    }
    XrReferenceSpaceCreateInfo spaceInfo{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    spaceInfo.poseInReferenceSpace.orientation.w = 1.0f;
    result = xrCreateReferenceSpace(session_, &spaceInfo, &space_);
    if (XR_FAILED(result)) {
        error = "xrCreateReferenceSpace failed: " + ResultString(result);
        return false;
    }
    uint32_t viewCount = 0;
    xrEnumerateViewConfigurationViews(instance_, systemId_, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
        0, &viewCount, nullptr);
    if (viewCount != 2) {
        error = "PRIMARY_STEREO did not return exactly two views";
        return false;
    }
    std::array<XrViewConfigurationView, 2> viewConfigurationViews{
        XrViewConfigurationView{XR_TYPE_VIEW_CONFIGURATION_VIEW},
        XrViewConfigurationView{XR_TYPE_VIEW_CONFIGURATION_VIEW}};
    xrEnumerateViewConfigurationViews(instance_, systemId_, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
        2, &viewCount, viewConfigurationViews.data());
    width_ = static_cast<int>(std::max(
        viewConfigurationViews[0].recommendedImageRectWidth,
        viewConfigurationViews[1].recommendedImageRectWidth));
    height_ = static_cast<int>(std::max(
        viewConfigurationViews[0].recommendedImageRectHeight,
        viewConfigurationViews[1].recommendedImageRectHeight));
    uint32_t formatCount = 0;
    xrEnumerateSwapchainFormats(session_, 0, &formatCount, nullptr);
    std::vector<int64_t> formats(formatCount);
    xrEnumerateSwapchainFormats(session_, formatCount, &formatCount, formats.data());
    const int64_t preferredFormats[] = {GL_SRGB8_ALPHA8, GL_RGBA8};
    int64_t selectedFormat = 0;
    for (int64_t preferred : preferredFormats) {
        if (std::find(formats.begin(), formats.end(), preferred) != formats.end()) {
            selectedFormat = preferred;
            break;
        }
    }
    if (!selectedFormat) {
        error = "OpenXR runtime exposes no RGBA8 OpenGL swapchain format";
        return false;
    }
    XrSwapchainCreateInfo swapchainInfo{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    swapchainInfo.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT |
        XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    swapchainInfo.format = selectedFormat;
    swapchainInfo.sampleCount = 1;
    swapchainInfo.width = width_;
    swapchainInfo.height = height_;
    swapchainInfo.faceCount = 1;
    swapchainInfo.arraySize = 2;
    swapchainInfo.mipCount = 1;
    result = xrCreateSwapchain(session_, &swapchainInfo, &swapchain_);
    if (XR_FAILED(result)) {
        error = "xrCreateSwapchain failed: " + ResultString(result);
        return false;
    }
    uint32_t imageCount = 0;
    xrEnumerateSwapchainImages(swapchain_, 0, &imageCount, nullptr);
    images_.assign(imageCount, XrSwapchainImageOpenGLKHR{XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR});
    result = xrEnumerateSwapchainImages(swapchain_, imageCount, &imageCount,
        reinterpret_cast<XrSwapchainImageBaseHeader*>(images_.data()));
    if (XR_FAILED(result)) {
        error = "xrEnumerateSwapchainImages failed: " + ResultString(result);
        return false;
    }
    return true;
}

void LinuxOpenXrBackend::PollEvents() {
    XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
    while (xrPollEvent(instance_, &event) == XR_SUCCESS) {
        if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            const auto* changed = reinterpret_cast<const XrEventDataSessionStateChanged*>(&event);
            if (changed->state == XR_SESSION_STATE_READY && !sessionRunning_) {
                XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO};
                begin.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                if (XR_SUCCEEDED(xrBeginSession(session_, &begin))) sessionRunning_ = true;
            } else if (changed->state == XR_SESSION_STATE_STOPPING && sessionRunning_) {
                xrEndSession(session_);
                sessionRunning_ = false;
            } else if (changed->state == XR_SESSION_STATE_EXITING ||
                       changed->state == XR_SESSION_STATE_LOSS_PENDING) {
                exitRequested_ = true;
            }
        } else if (event.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
            exitRequested_ = true;
        }
        event = {XR_TYPE_EVENT_DATA_BUFFER};
    }
}

bool LinuxOpenXrBackend::BeginFrame(Frame& frame, std::string& error) {
    PollEvents();
    if (!sessionRunning_) return false;
    XrFrameWaitInfo waitInfo{XR_TYPE_FRAME_WAIT_INFO};
    XrResult result = xrWaitFrame(session_, &waitInfo, &frameState_);
    if (XR_FAILED(result)) { error = "xrWaitFrame failed: " + ResultString(result); return false; }
    XrFrameBeginInfo beginInfo{XR_TYPE_FRAME_BEGIN_INFO};
    result = xrBeginFrame(session_, &beginInfo);
    if (XR_FAILED(result)) { error = "xrBeginFrame failed: " + ResultString(result); return false; }
    frameBegun_ = true;
    if (!frameState_.shouldRender) return false;
    XrViewLocateInfo locateInfo{XR_TYPE_VIEW_LOCATE_INFO};
    locateInfo.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    locateInfo.displayTime = frameState_.predictedDisplayTime;
    locateInfo.space = space_;
    XrViewState viewState{XR_TYPE_VIEW_STATE};
    uint32_t viewCount = 0;
    views_ = {XrView{XR_TYPE_VIEW}, XrView{XR_TYPE_VIEW}};
    result = xrLocateViews(session_, &locateInfo, &viewState, 2, &viewCount, views_.data());
    if (XR_FAILED(result) || viewCount != 2) {
        error = "xrLocateViews failed to return two views: " + ResultString(result);
        return false;
    }
    constexpr XrViewStateFlags requiredFlags =
        XR_VIEW_STATE_POSITION_VALID_BIT | XR_VIEW_STATE_ORIENTATION_VALID_BIT;
    if ((viewState.viewStateFlags & requiredFlags) != requiredFlags) {
        error = "OpenXR runtime returned invalid stereo view poses";
        return false;
    }
    XrSwapchainImageAcquireInfo acquireInfo{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    result = xrAcquireSwapchainImage(swapchain_, &acquireInfo, &acquiredImage_);
    if (XR_FAILED(result)) { error = "xrAcquireSwapchainImage failed: " + ResultString(result); return false; }
    imageAcquired_ = true;
    XrSwapchainImageWaitInfo waitImage{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    waitImage.timeout = XR_INFINITE_DURATION;
    result = xrWaitSwapchainImage(swapchain_, &waitImage);
    if (XR_FAILED(result)) { error = "xrWaitSwapchainImage failed: " + ResultString(result); return false; }
    frame.colorTexture = images_[acquiredImage_].image;
    frame.width = width_;
    frame.height = height_;
    frame.views = views_;
    return true;
}

bool LinuxOpenXrBackend::EndFrame(std::string& error) {
    if (!frameBegun_) return true;
    if (imageAcquired_) {
        XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        const XrResult releaseResult = xrReleaseSwapchainImage(swapchain_, &release);
        imageAcquired_ = false;
        if (XR_FAILED(releaseResult)) {
            error = "xrReleaseSwapchainImage failed: " + ResultString(releaseResult);
            return false;
        }
    }
    std::array<XrCompositionLayerProjectionView, 2> projectionViews{
        XrCompositionLayerProjectionView{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
        XrCompositionLayerProjectionView{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
    for (uint32_t eye = 0; eye < projectionViews.size(); ++eye) {
        projectionViews[eye].pose = views_[eye].pose;
        projectionViews[eye].fov = views_[eye].fov;
        projectionViews[eye].subImage.swapchain = swapchain_;
        projectionViews[eye].subImage.imageRect.extent = {width_, height_};
        projectionViews[eye].subImage.imageArrayIndex = eye;
    }
    XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    layer.space = space_;
    layer.viewCount = 2;
    layer.views = projectionViews.data();
    const XrCompositionLayerBaseHeader* layers[] = {
        reinterpret_cast<const XrCompositionLayerBaseHeader*>(&layer)};
    XrFrameEndInfo endInfo{XR_TYPE_FRAME_END_INFO};
    endInfo.displayTime = frameState_.predictedDisplayTime;
    endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    endInfo.layerCount = frameState_.shouldRender ? 1u : 0u;
    endInfo.layers = frameState_.shouldRender ? layers : nullptr;
    const XrResult result = xrEndFrame(session_, &endInfo);
    frameBegun_ = false;
    if (XR_FAILED(result)) { error = "xrEndFrame failed: " + ResultString(result); return false; }
    return true;
}

void LinuxOpenXrBackend::Shutdown() {
    if (sessionRunning_) xrEndSession(session_);
    if (swapchain_ != XR_NULL_HANDLE) xrDestroySwapchain(swapchain_);
    if (space_ != XR_NULL_HANDLE) xrDestroySpace(space_);
    if (session_ != XR_NULL_HANDLE) xrDestroySession(session_);
    if (instance_ != XR_NULL_HANDLE) xrDestroyInstance(instance_);
    swapchain_ = XR_NULL_HANDLE; space_ = XR_NULL_HANDLE; session_ = XR_NULL_HANDLE;
    instance_ = XR_NULL_HANDLE;
}
