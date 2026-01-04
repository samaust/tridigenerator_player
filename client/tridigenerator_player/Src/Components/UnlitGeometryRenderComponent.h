#pragma once

#include <GLES3/gl3.h>
#include <string>

#include "OVR_Math.h"

// Texture indices for clarity
enum TextureSlot {
    TEX_Y = 0,
    TEX_U = 1,
    TEX_V = 2,
    TEX_ALPHA = 3,
    TEX_DEPTH = 4,
    TEX_ENV_DEPTH = 5,
    TEXTURE_SLOT_MAX = 6
};

struct UnlitGeometryRenderComponent {
    // Texture and rendering related
    GLenum texture_internal_formats_[TEXTURE_SLOT_MAX] = {
            GL_R8,      // For TEX_Y
            GL_R8,      // For TEX_U
            GL_R8,      // For TEX_V
            GL_R8,      // For TEX_ALPHA
            GL_R16UI,   // For TEX_DEPTH
            0           // For TEX_ENV_DEPTH (external)
    };
    int texture_unpack_alignments_[TEXTURE_SLOT_MAX] = {1, 1, 1, 1, 1, 1};
    uint32_t BlendSrc = OVRFW::ovrGpuState::kGL_SRC_ALPHA;
    uint32_t BlendDst = OVRFW::ovrGpuState::kGL_ONE_MINUS_SRC_ALPHA;
    uint32_t BlendMode = OVRFW::ovrGpuState::kGL_FUNC_ADD;

    // Pose related
    bool poseInitialized = false;
    std::string poseParent = "HeadPose";
    OVR::Vector3f poseTranslationOffset = OVR::Vector3f(0.0f, 0.0f, 0.0f);
};
