#pragma once

#include <vector>
#include <string>

#include "OVR_Math.h"

#include "Render/GlTexture.h"
#include "Render/SurfaceRender.h"
#include "Render/GlProgram.h"

#include "../Components/UnlitGeometryRenderComponent.h"

struct UnlitGeometryRenderState {
    // Double-buffered textures
    OVRFW::GlTexture textures_[2][TEXTURE_SLOT_MAX];

    // Double-buffered surface definitions
    OVRFW::ovrSurfaceDef surfaceDefs_[2];

    // Which surface set is currently being used for rendering
    int currentSurfaceSet_ = 0;

    // Shader programs (limited-range and full-range YUV)
    OVRFW::GlProgram ProgramLimited_;
    OVRFW::GlProgram ProgramFullRange_;
    int useFullRangeYuv_ = 0;

    OVR::Vector4f intrinsics_ = OVR::Vector4f(1.0f, 1.0f, 0.0f, 0.0f);
    OVR::Vector2f imageSize_ = OVR::Vector2f(1.0f, 1.0f);
    int hasEnvironmentDepth_ = 0;
    OVR::Vector2f environmentDepthTexelSize_ = OVR::Vector2f(0.0f, 0.0f);
    OVR::Vector4f occlusionParams_ = OVR::Vector4f(1.0f, 0.01f, 0.0025f, 0.0f);
    OVR::Matrix4f lightParams_;
    OVR::Vector4f matchingLimits_ = OVR::Vector4f(0.7f, 1.4f, 0.35f, 2.0f);
    OVRFW::GlTexture datasetReferenceTexture_;
    std::string datasetReferenceSequence_;
    int datasetReferenceSchemaVersion_ = 0;
};
