#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include <X11/Xlib.h>
#include <GL/glx.h>

#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

class LinuxOpenXrBackend {
public:
    struct Frame {
        uint32_t colorTexture = 0;
        int width = 0;
        int height = 0;
        std::array<XrView, 2> views{
            XrView{XR_TYPE_VIEW}, XrView{XR_TYPE_VIEW}};
    };

    LinuxOpenXrBackend() = default;
    ~LinuxOpenXrBackend();
    LinuxOpenXrBackend(const LinuxOpenXrBackend&) = delete;
    LinuxOpenXrBackend& operator=(const LinuxOpenXrBackend&) = delete;

    bool Initialize(
        Display* display,
        GLXFBConfig framebufferConfig,
        GLXDrawable drawable,
        GLXContext context,
        std::string& error);
    bool BeginFrame(Frame& frame, std::string& error);
    bool EndFrame(std::string& error);
    bool ExitRequested() const { return exitRequested_; }

private:
    void PollEvents();
    void Shutdown();
    std::string ResultString(XrResult result) const;

    XrInstance instance_ = XR_NULL_HANDLE;
    XrSystemId systemId_ = XR_NULL_SYSTEM_ID;
    XrSession session_ = XR_NULL_HANDLE;
    XrSpace space_ = XR_NULL_HANDLE;
    XrSwapchain swapchain_ = XR_NULL_HANDLE;
    std::vector<XrSwapchainImageOpenGLKHR> images_;
    XrFrameState frameState_{XR_TYPE_FRAME_STATE};
    std::array<XrView, 2> views_{XrView{XR_TYPE_VIEW}, XrView{XR_TYPE_VIEW}};
    uint32_t acquiredImage_ = 0;
    int width_ = 0;
    int height_ = 0;
    bool sessionRunning_ = false;
    bool frameBegun_ = false;
    bool imageAcquired_ = false;
    bool exitRequested_ = false;
};
