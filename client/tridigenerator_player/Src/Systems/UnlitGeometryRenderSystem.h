#pragma once

#include <GLES3/gl3.h>
#include <memory>
#include <utility>

#include "FrameParams.h"
#include "Render/GlGeometry.h"
#include "Render/GlTexture.h"
#include "Render/VideoFrame.h"

#include "OVR_Math.h"

#include "../Core/EntityManager.h"
#include "../Data/GpuTimingManager.h"
#include "../Data/PerformanceTimingStats.h"

#include "../Components/FrameLoaderComponent.h"
#include "../Components/TransformComponent.h"
#include "../Components/InteractableComponent.h"
#include "../Components/UnlitGeometryRenderComponent.h"

#include "../States/TransformState.h"
#include "../States/UnlitGeometryRenderState.h"

struct EnvironmentDepthState;

class UnlitGeometryRenderSystem {
public:
    explicit UnlitGeometryRenderSystem(
        std::shared_ptr<PerformanceTimingStats> performanceTiming = nullptr,
        GpuTimingManager* gpuTiming = nullptr)
        : performanceTiming_(std::move(performanceTiming)),
          gpuTiming_(gpuTiming) {}

    bool Init(EntityManager& ecs, int meshDetailDivisor = 2);
    bool RebuildGeometry(EntityManager& ecs, int meshDetailDivisor);
    void Shutdown(EntityManager& ecs);
    void Update(EntityManager& ecs, const OVRFW::ovrApplFrameIn &in);
    bool TexturesCreated(UnlitGeometryRenderState &ugrS) const;
    void CreateTextures(
            VideoFrame** framePtr,
            UnlitGeometryRenderComponent &ugrC,
            UnlitGeometryRenderState &ugrS);
    bool RecreateDepthTextures(
            UnlitGeometryRenderComponent& ugrC,
            UnlitGeometryRenderState& ugrS);
    OVRFW::GlTexture CreateGlTexture(GLenum internalformat, uint32_t pixelWidth, uint32_t pixelHeight);
    void UpdateFrameGeometry(
            const FrameLoaderComponent& flC,
            const VideoFrame& frame,
            TransformComponent& transform,
            TransformState& transformState,
            UnlitGeometryRenderState& renderState,
            InteractableComponent& interactable);
    void UpdateDepthScaleFactor(FrameLoaderComponent &flC, UnlitGeometryRenderState &ugrS);
    bool UpdateTextures(
            UnlitGeometryRenderComponent &ugrC,
            VideoFrame** framePtr,
            UnlitGeometryRenderState &ugrS);
    bool UpdateDynamicIndices(
            const FrameLoaderComponent& loader,
            VideoFrame& frame,
            UnlitGeometryRenderState& renderState);
    void BindFullIndexBuffer(UnlitGeometryRenderState& renderState);
    void UpdateGl8Texture(
            OVRFW::GlTexture texture, GLenum format,
            const void* textureData,
            int unpack_alignment,
            int stride);
    void UpdateGl16Texture(
            OVRFW::GlTexture texture, GLenum format,
            const void* textureData,
            int unpack_alignment,
            int stride);
    void UpdateEnvironmentDepthUniforms(
        UnlitGeometryRenderComponent& ugrC,
        UnlitGeometryRenderState& ugrS,
        EnvironmentDepthState* environmentDepthState);
    void Render(EntityManager& ecs, std::vector<OVRFW::ovrDrawSurface>& surfaceList);

private:
    std::shared_ptr<PerformanceTimingStats> performanceTiming_;
    GpuTimingManager* gpuTiming_ = nullptr;
};
