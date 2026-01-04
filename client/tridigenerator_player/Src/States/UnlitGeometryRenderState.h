#pragma once

#include <vector>

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

    // Shader program used for rendering
    OVRFW::GlProgram Program_;

    float fovX_rad;
    float fovY_rad;
    int hasEnvironmentDepth_ = 0;
    OVR::Vector2f environmentDepthTexelSize_ = OVR::Vector2f(0.0f, 0.0f);
};
