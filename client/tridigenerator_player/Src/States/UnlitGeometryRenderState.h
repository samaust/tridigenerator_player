#pragma once

#import "Render/GlTexture.h"
#import "Render/SurfaceRender.h"
#import "Render/GlProgram.h"

#import "../Components/UnlitGeometryRenderComponent.h"

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
};
