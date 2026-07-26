#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "OVR_Math.h"

#include "Render/GlTexture.h"
#include "Render/SurfaceRender.h"
#include "Render/GlProgram.h"

#include "../Components/UnlitGeometryRenderComponent.h"

struct UnlitGeometryRenderState {
    struct HardwareColorUploadLease {
        std::shared_ptr<void> owner;
        void* display = nullptr;
        void* image = nullptr;
        void* fence = nullptr;
    };

    // Double-buffered textures
    OVRFW::GlTexture textures_[2][TEXTURE_SLOT_MAX];

    // Double-buffered surface definitions
    OVRFW::ovrSurfaceDef surfaceDefs_[2];

    // Which surface set is currently being used for rendering
    int currentSurfaceSet_ = 0;

    int meshDetailDivisor_ = 2;
    uint32_t meshWidth_ = 2;
    uint32_t meshHeight_ = 2;
    bool depthTextureReady_ = false;
    unsigned fullIndexBuffer_ = 0;
    int fullIndexCount_ = 0;
    bool usingDynamicIndices_ = false;
    uint64_t dynamicVisibilityVersion_ = 0;
    uint32_t retainedCellCount_ = 0;
    uint32_t rejectedCellCount_ = 0;
    uint32_t mixedCellCount_ = 0;
    size_t compactIndexBytes_ = 0;

    static constexpr size_t UploadPboCount = 3;
    std::array<unsigned, UploadPboCount> uploadPbos_{};
    size_t nextUploadPbo_ = 0;
    bool pboFailureLogged_ = false;

    // Android hardware-color conversion state. EGL/GL objects are stored as
    // opaque values here so this state remains buildable on desktop.
    unsigned hardwareColorProgram_ = 0;
    unsigned hardwareColorFramebuffer_ = 0;
    unsigned hardwareColorExternalTexture_ = 0;
    unsigned hardwareColorVertexArray_ = 0;
    void* getNativeClientBufferProc_ = nullptr;
    void* imageTargetTextureProc_ = nullptr;
    void* createImageProc_ = nullptr;
    void* destroyImageProc_ = nullptr;
    std::array<HardwareColorUploadLease, 2> hardwareColorLeases_{};
    uint64_t hardwareColorDroppedFrames_ = 0;

    // Shader programs (limited-range and full-range YUV)
    OVRFW::GlProgram ProgramLimited_;
    OVRFW::GlProgram ProgramFullRange_;
    OVRFW::GlProgram ProgramGlobalHardLimited_;
    OVRFW::GlProgram ProgramGlobalHardFullRange_;
    int useFullRangeYuv_ = 0;
    int colorIsRgb_ = 0;
    bool useGlobalHardVariant_ = false;

    OVR::Vector4f intrinsics_ = OVR::Vector4f(1.0f, 1.0f, 0.0f, 0.0f);
    OVR::Vector2f imageSize_ = OVR::Vector2f(1.0f, 1.0f);
    int hasEnvironmentDepth_ = 0;
    OVR::Vector2f environmentDepthTexelSize_ = OVR::Vector2f(0.0f, 0.0f);
    OVR::Vector4f occlusionParams_ = OVR::Vector4f(1.0f, 0.01f, 0.0025f, 0.0f);
    OVR::Matrix4f lightParams_;
    OVR::Vector4f matchingLimits_ = OVR::Vector4f(0.7f, 1.4f, 0.05f, 2.0f);
    OVRFW::GlTexture datasetReferenceTexture_;
    std::string datasetReferenceSequence_;
    int datasetReferenceSchemaVersion_ = 0;
};
