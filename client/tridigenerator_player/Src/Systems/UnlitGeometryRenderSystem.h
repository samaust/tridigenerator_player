#pragma once

#include <GLES3/gl3.h>

#include "FrameParams.h"
#include "Render/GlGeometry.h"
#include "Render/GlTexture.h"
#include "Render/VideoFrame.h"

#include "OVR_Math.h"

#include "../Core/EntityManager.h"

#include "../Components/FrameLoaderComponent.h"
#include "../Components/UnlitGeometryRenderComponent.h"

#include "../States/UnlitGeometryRenderState.h"

struct EnvironmentDepthState;

class UnlitGeometryRenderSystem {
public:
    bool Init(EntityManager& ecs);
    void Shutdown(EntityManager& ecs);
    void Update(EntityManager& ecs, const OVRFW::ovrApplFrameIn &in);
    bool TexturesCreated(UnlitGeometryRenderState &ugrS) const;
    void CreateTextures(
            VideoFrame** framePtr,
            UnlitGeometryRenderComponent &ugrC,
            UnlitGeometryRenderState &ugrS);
    OVRFW::GlTexture CreateGlTexture(GLenum internalformat, uint32_t pixelWidth, uint32_t pixelHeight);
    void UpdateFov(float fovX_deg, UnlitGeometryRenderState &ugrS);
    void UpdateDepthScaleFactor(FrameLoaderComponent &flC, UnlitGeometryRenderState &ugrS);
    void UpdateTextures(
            UnlitGeometryRenderComponent &ugrC,
            VideoFrame** framePtr,
            UnlitGeometryRenderState &ugrS);
    void UpdateGlTexture(
            OVRFW::GlTexture texture, GLenum format,
            const uint8_t* textureData,
            int unpack_alignment,
            int stride);
    void UpdateGlTexture(
            OVRFW::GlTexture texture, GLenum format,
            const uint16_t* textureData,
            int unpack_alignment,
            int stride);
    void UpdateEnvironmentDepthUniforms(UnlitGeometryRenderState& ugrS, EnvironmentDepthState* environmentDepthState);
    void Render(EntityManager& ecs, std::vector<OVRFW::ovrDrawSurface>& surfaceList);
};
