/**
 * @file UnlitGeometryRenderSystem.cpp
 * @brief Rendering system for unlit geometry with YUV texture support.
 * 
 * This system manages the complete rendering pipeline for unlit geometric objects
 * using YUV color format with alpha and depth textures. It implements double-buffering
 * for smooth texture updates without rendering artifacts.
 * 
 * Key Features:
 * - YUV420 texture format support (Y, U, V channels) with alpha channel
 * - 16-bit depth texture support for depth-based effects
 * - Double-buffered texture sets to prevent tearing during updates
 * - Configurable blend modes and GPU state management
 * - Dynamic FOV calculation based on aspect ratio
 * - Depth scale factor uniform for vertex shader depth calculations
 * 
 * System Components:
 * - Init(): Initializes geometry and shader programs
 * - Update(): Manages texture creation, updates, and uniform synchronization
 * - Render(): Submits the prepared geometry to the render queue
 * - Shutdown(): Cleans up GPU resources
 * 
 * Texture Management:
 * - CreateTextures(): Allocates immutable OpenGL texture storage
 * - UpdateTextures(): Uploads new frame data to GPU using double-buffering
 * - UpdateGlTexture(): Handles texture data upload with stride and alignment support
 * 
 * Shader Uniforms:
 * - u_texY, u_texU, u_texV, u_texAlpha, u_texDepth: Sampler uniforms
 * - u_FovX_rad, u_FovY_rad: Field of view in radians
 * - u_depthScaleFactor: Scale factor for depth value conversion
 */
#include "UnlitGeometryRenderSystem.h"

#include "Render/GeometryBuilder.h"
#include "Render/gl_pixel_format.h"

#include "../Shaders/UnlitGeometryRenderShaders.h"

#include "../Systems/TransformSystem.h"

#include "../Components/TransformComponent.h"

#include "../States/TransformState.h"
#include "../States/FrameLoaderState.h"

using OVR::Matrix4f;
using OVR::Posef;
using OVR::Quatf;
using OVR::Vector2f;
using OVR::Vector3f;
using OVR::Vector4f;

#include "../Core/Logging.h"


/**
 * @brief Initialize the unlit geometry render system for entities that include
 *        FrameLoader and UnlitGeometryRenderState components.
 *
 * This function builds the base geometry (a tesselated quad), creates the
 * shader program used for YUV + alpha + depth rendering, and prepares two
 * surface definitions (double-buffered) with appropriate GPU state.
 *
 * @param ecs Reference to the entity manager used to iterate entities.
 * @return true on successful initialization, false otherwise.
 */
bool UnlitGeometryRenderSystem::Init(EntityManager& ecs) {
    ecs.ForEachMulti<UnlitGeometryRenderState, FrameLoaderComponent>(
            [&](EntityID e,
                     UnlitGeometryRenderState& ugrS,
                     FrameLoaderComponent& flC) {
        // Create initial plane geometry and renderer
        auto planeDescriptor = OVRFW::BuildTesselatedQuadDescriptor(
                flC.width-1,
                flC.height-1,
                true,
                false);
        OVR::Vector4f planeColor = {1.0f, 0.0f, 0.0f, 1.0f};
        OVRFW::GeometryBuilder planeGeometry;
        planeGeometry.Add(
                planeDescriptor,
                OVRFW::GeometryBuilder::kInvalidIndex,
                planeColor);

        auto d = planeGeometry.ToGeometryDescriptor();
        //d.attribs.position = frame.positions;
        //d.attribs.color = frame.colors;

        /// Program
        static OVRFW::ovrProgramParm GeometryUniformParms[] = {
                {"u_texY", OVRFW::ovrProgramParmType::TEXTURE_SAMPLED},
                {"u_texU", OVRFW::ovrProgramParmType::TEXTURE_SAMPLED},
                {"u_texV", OVRFW::ovrProgramParmType::TEXTURE_SAMPLED},
                {"u_texAlpha", OVRFW::ovrProgramParmType::TEXTURE_SAMPLED},
                {"u_texDepth", OVRFW::ovrProgramParmType::TEXTURE_SAMPLED},
                {"u_FovX_rad", OVRFW::ovrProgramParmType::FLOAT},
                {"u_FovY_rad", OVRFW::ovrProgramParmType::FLOAT},
                {"u_depthScaleFactor", OVRFW::ovrProgramParmType::FLOAT},
        };

        std::string programDefs;

        // Initialize BOTH surface definitions
        for (int i = 0; i < 2; ++i) {
            ugrS.surfaceDefs_[i].geo = OVRFW::GlGeometry(d.attribs, d.indices);

            ugrS.Program_ = OVRFW::GlProgram::Build(
                    programDefs.c_str(),
                    UnlitGeometryVertexShaderSrc,
                    programDefs.c_str(),
                    UnlitGeometryFragmentShaderSrc,
                    GeometryUniformParms,
                    sizeof(GeometryUniformParms) / sizeof(OVRFW::ovrProgramParm));

            /// Hook the graphics command
            OVRFW::ovrGraphicsCommand &gc = ugrS.surfaceDefs_[i].graphicsCommand;
            gc.Program = ugrS.Program_;

            /// gpu state needs alpha blending
            gc.GpuState.depthEnable = gc.GpuState.depthMaskEnable = true;
            gc.GpuState.blendEnable = OVRFW::ovrGpuState::BLEND_ENABLE;
        }
    });
    return true;
}

/**
 * @brief Shutdown and free GPU resources owned by the render system.
 *
 * Frees any textures, shader programs, and geometry allocated in the
 * UnlitGeometryRenderState for each entity.
 *
 * @param ecs Reference to the entity manager used to iterate entities.
 */
void UnlitGeometryRenderSystem::Shutdown(EntityManager& ecs) {
    ecs.ForEach<UnlitGeometryRenderState>(
            [&](EntityID e, UnlitGeometryRenderState& ugrS) {
        for (int i = 0; i < 2; ++i) {
            OVRFW::FreeTexture(ugrS.textures_[i][TEX_Y]);
            OVRFW::FreeTexture(ugrS.textures_[i][TEX_U]);
            OVRFW::FreeTexture(ugrS.textures_[i][TEX_V]);
            OVRFW::FreeTexture(ugrS.textures_[i][TEX_ALPHA]);
            OVRFW::FreeTexture(ugrS.textures_[i][TEX_DEPTH]);
        }
        OVRFW::GlProgram::Free(ugrS.Program_);
                ugrS.surfaceDefs_[0].geo.Free();
                ugrS.surfaceDefs_[1].geo.Free();
    });
}

/**
 * @brief Per-frame update for the render system.
 *
 * Handles pose initialization, model matrix updates, texture creation when a
 * new frame is available, and triggers texture uploads using double-buffering.
 *
 * @param ecs Reference to the entity manager used to iterate entities.
 * @param in The per-frame input data (head pose, timing, etc.).
 */
void UnlitGeometryRenderSystem::Update(EntityManager& ecs, const OVRFW::ovrApplFrameIn &in) {
    ecs.ForEachMulti<TransformComponent,
                     TransformState,
                     FrameLoaderComponent,
                     FrameLoaderState,
                     UnlitGeometryRenderComponent,
                     UnlitGeometryRenderState>(
        [&](EntityID e,
                 TransformComponent &tC,
                 TransformState &tS,
                 FrameLoaderComponent &flC,
                 FrameLoaderState &flS,
                 UnlitGeometryRenderComponent &ugrC,
                 UnlitGeometryRenderState &ugrS) {
        // Initialize pose if needed
        if (!ugrC.poseInitialized) {
            LOGI("Initialising pose of entity");
            if (ugrC.poseParent == "HeadPose") {
                // Might contain errors
                const OVR::Posef headPose = in.HeadPose;
                OVR::Posef combinedPose = headPose;
                combinedPose.Translation = headPose.Translate(ugrC.poseTranslationOffset);
                LOGI("  HeadPose Rotation: (%f, %f, %f, %f)",
                     headPose.Rotation.x,
                     headPose.Rotation.y,
                     headPose.Rotation.z,
                     headPose.Rotation.w);
                LOGI("  combinedPose Rotation: (%f, %f, %f, %f)",
                     combinedPose.Rotation.x,
                     combinedPose.Rotation.y,
                     combinedPose.Rotation.z,
                     combinedPose.Rotation.w);

                LOGI("  HeadPose Translation: (%f, %f, %f)",
                     headPose.Translation.x,
                     headPose.Translation.y,
                     headPose.Translation.z);
                LOGI("  combinedPose Translation: (%f, %f, %f)",
                     combinedPose.Translation.x,
                     combinedPose.Translation.y,
                     combinedPose.Translation.z);
                TransformSystem::SetPose(tC, tS, combinedPose);
            }
            ugrC.poseInitialized = true;
        }

        // Update model matrix
        tC.modelPose.Rotation.Normalize();
        tS.modelMatrix = OVR::Matrix4f(tC.modelPose) * OVR::Matrix4f::Scaling(tC.modelScale);

        // Create textures if not already created
        if (!TexturesCreated(ugrS) && flS.framePtr != nullptr) {
            LOGI("Creating textures");
            CreateTextures(
                    flS.framePtr,
                    ugrC,
                    ugrS);
            UpdateFov(flC.fovX_deg, ugrS);
            UpdateDepthScaleFactor(flC, ugrS);
        }

        if (flS.frameReady.load(std::memory_order_acquire)) {
            //LOGI("Update textures with new frame");
            // A new frame is available, so update textures.
            UpdateTextures(ugrC, flS.framePtr, ugrS);

            // Consume the flag by setting it back to false.
            flS.frameReady.store(false, std::memory_order_relaxed);
        }
    });
}

/**
 * @brief Returns whether the initial set of textures have been created.
 *
 * Checks the first texture set (index 0) for non-zero GL texture handles
 * for Y, U, V, alpha and depth textures.
 *
 * @param ugrS The render state to inspect.
 * @return true if all required textures are present, false otherwise.
 */
bool UnlitGeometryRenderSystem::TexturesCreated(UnlitGeometryRenderState &ugrS) const {
    bool created = (ugrS.textures_[0][TEX_Y].texture != 0 &&
                    ugrS.textures_[0][TEX_U].texture != 0 &&
                    ugrS.textures_[0][TEX_V].texture != 0 &&
                    ugrS.textures_[0][TEX_ALPHA].texture != 0 &&
                    ugrS.textures_[0][TEX_DEPTH].texture != 0);
    return created;
}

/**
 * @brief Creates and initializes the OpenGL textures needed for rendering.
 *
 * Allocates immutable GL textures for Y, U, V, alpha and 16-bit depth for
 * both double-buffered sets, binds them to the surface definitions and
 * computes unpack alignment values used for later uploads.
 *
 * @param framePtr Pointer to a pointer to a VideoFrame that contains texture
 *                 dimensions and data buffers used to size the textures.
 * @param ugrC Component configuration containing texture internal formats
 *             and where unpack alignment values will be stored.
 * @param ugrS State that will receive created textures and surface bindings.
 */
void UnlitGeometryRenderSystem::CreateTextures(
        VideoFrame** framePtr,
        UnlitGeometryRenderComponent &ugrC,
        UnlitGeometryRenderState &ugrS) {
    // Validate frame pointer
    if (framePtr == nullptr || *framePtr == nullptr) {
        LOGE("CreateTextures called with null framePtr");
        return;
    }

    LOGI("Creating textures with frame dimensions:");
    LOGI("  Y: %d x %d", (*framePtr)->textureYWidth, (*framePtr)->textureYHeight);
    LOGI("  U: %d x %d", (*framePtr)->textureUWidth, (*framePtr)->textureUHeight);
    LOGI("  V: %d x %d", (*framePtr)->textureVWidth, (*framePtr)->textureVHeight);
    LOGI("  Alpha: %d x %d", (*framePtr)->textureAlphaWidth, (*framePtr)->textureAlphaHeight);
    LOGI("  Depth: %d x %d", (*framePtr)->textureDepthWidth, (*framePtr)->textureDepthHeight);

    // Create textures for both sets
    for (int i = 0; i < 2; ++i) {
        // YUV textures
        ugrS.textures_[i][TEX_Y] = CreateGlTexture(
            ugrC.texture_internal_formats_[TEX_Y],
            (*framePtr)->textureYWidth,
            (*framePtr)->textureYHeight);
        ugrS.textures_[i][TEX_U] = CreateGlTexture(
            ugrC.texture_internal_formats_[TEX_U],
            (*framePtr)->textureUWidth,
            (*framePtr)->textureUHeight);
        ugrS.textures_[i][TEX_V] = CreateGlTexture(
            ugrC.texture_internal_formats_[TEX_V],
            (*framePtr)->textureVWidth,
            (*framePtr)->textureVHeight);

        // Alpha texture
        ugrS.textures_[i][TEX_ALPHA] = CreateGlTexture(
            ugrC.texture_internal_formats_[TEX_ALPHA],
            (*framePtr)->textureAlphaWidth,
            (*framePtr)->textureAlphaHeight);

        // Create one 16-bit textures for the 16-bit depth data
        ugrS.textures_[i][TEX_DEPTH] = CreateGlTexture(
            ugrC.texture_internal_formats_[TEX_DEPTH],
            (*framePtr)->textureDepthWidth,
            (*framePtr)->textureDepthHeight);

        // --- Assign textures to their corresponding surface def ---
        OVRFW::ovrGraphicsCommand &gc = ugrS.surfaceDefs_[i].graphicsCommand;
        gc.Textures[TEX_Y] = ugrS.textures_[i][TEX_Y];
        gc.Textures[TEX_U] = ugrS.textures_[i][TEX_U];
        gc.Textures[TEX_V] = ugrS.textures_[i][TEX_V];
        gc.Textures[TEX_ALPHA] = ugrS.textures_[i][TEX_ALPHA];
        gc.Textures[TEX_DEPTH] = ugrS.textures_[i][TEX_DEPTH];
        gc.BindUniformTextures();
    }

    // Compute bytes per row
    const uint32_t tex_Y_bpr = (*framePtr)->textureYWidth * bytesPerPixel(ugrC.texture_internal_formats_[TEX_Y]);
    const uint32_t tex_U_bpr = (*framePtr)->textureUWidth * bytesPerPixel(ugrC.texture_internal_formats_[TEX_U]);
    const uint32_t tex_V_bpr = (*framePtr)->textureVWidth * bytesPerPixel(ugrC.texture_internal_formats_[TEX_V]);
    const uint32_t tex_alpha_bpr = (*framePtr)->textureAlphaWidth * bytesPerPixel(ugrC.texture_internal_formats_[TEX_ALPHA]);
    const uint32_t tex_depth_bpr = (*framePtr)->textureDepthWidth * bytesPerPixel(ugrC.texture_internal_formats_[TEX_DEPTH]);

    // Set unpack alignments based on the frame data strides
    // Another choice is to use stride values from frame directly
    ugrC.texture_unpack_alignments_[TEX_Y] = computeUnpackAlignment(tex_Y_bpr);
    ugrC.texture_unpack_alignments_[TEX_U] = computeUnpackAlignment(tex_U_bpr);
    ugrC.texture_unpack_alignments_[TEX_V] = computeUnpackAlignment(tex_V_bpr);
    ugrC.texture_unpack_alignments_[TEX_ALPHA] = computeUnpackAlignment(tex_alpha_bpr);
    ugrC.texture_unpack_alignments_[TEX_DEPTH] = computeUnpackAlignment(tex_depth_bpr);

    // Start with set 0 as the one to be rendered.
    ugrS.currentSurfaceSet_ = 0;
}

/**
 * @brief Creates an immutable OpenGL 2D texture with given internal format.
 *
 * Configures basic sampling and wrap state and allocates immutable storage
 * using glTexStorage2D.
 *
 * @param internalformat GL internal format (e.g. GL_R8, GL_R16UI).
 * @param pixelWidth Width of the texture in pixels.
 * @param pixelHeight Height of the texture in pixels.
 * @return An OVRFW::GlTexture representing the created GL texture.
 */
OVRFW::GlTexture UnlitGeometryRenderSystem::CreateGlTexture(
        GLenum internalformat, uint32_t pixelWidth, uint32_t pixelHeight) {
    GLuint texId;
    glGenTextures(1, &texId);
    glBindTexture(GL_TEXTURE_2D, texId);

    if (internalformat == GL_R16UI) {
        // Integer textures (depth) MUST use NEAREST filtering.
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    } else {
        // Normalized textures (color/alpha) can use LINEAR.
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    }

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    // Allocate immutable storage
    glTexStorage2D(GL_TEXTURE_2D, 1, internalformat, pixelWidth, pixelHeight);

    return OVRFW::GlTexture(texId, GL_TEXTURE_2D, pixelWidth, pixelHeight);
}

/**
 * @brief Update cached horizontal and vertical field-of-view values.
 *
 * Converts horizontal FOV in degrees to radians, computes vertical FOV based
 * on the aspect ratio of the prepared textures and stores the values in the
 * UnlitGeometryRenderState as well as updating the renderer uniform pointers.
 *
 * @param fovX_deg Horizontal field of view in degrees (full-angle).
 * @param ugrS The render state to update (stores fov values and uniforms).
 */
void UnlitGeometryRenderSystem::UpdateFov(float fovX_deg, UnlitGeometryRenderState &ugrS) {
    float aspect_ratio = 1.0f;
    if (ugrS.textures_[0][0].Width != 0 && ugrS.textures_[0][0].Height != 0) {
        aspect_ratio = (float)ugrS.textures_[0][0].Height / (float)ugrS.textures_[0][0].Width;
    }

    ugrS.fovX_rad = fovX_deg * (M_PI / 180.0f) / 2.0f;

    // This is the correct formula to derive vertical FOV from horizontal FOV and aspect ratio
    ugrS.fovY_rad = atan(tan(ugrS.fovX_rad) * aspect_ratio);

    // Set the uniform values on the renderer's graphics command
    for (int i = 0; i < 2; ++i) {
        // u_FovX_rad
        ugrS.surfaceDefs_[i].graphicsCommand.UniformData[5].Data = &ugrS.fovX_rad;
        // u_FovY_rad
        ugrS.surfaceDefs_[i].graphicsCommand.UniformData[6].Data = &ugrS.fovY_rad;
    }
    LOGI("Updated FOV: fovX_rad=%f, fovY_rad=%f", ugrS.fovX_rad, ugrS.fovY_rad);
}

/**
 * @brief Updates the depth scale factor used in the vertex shader.
 *
 * The 16-bit depth texture stores values that must be scaled to meters in
 * the shader. This function updates the uniform pointer for both surface
 * definitions so the shader sees the new scale factor.
 *
 * @param flC Frame loader component that contains the new `depthScaleFactor`.
 * @param ugrS The render state whose uniform pointers will be updated.
 */
void UnlitGeometryRenderSystem::UpdateDepthScaleFactor(
    FrameLoaderComponent &flC,
    UnlitGeometryRenderState &ugrS) {
    // Update the uniform data in both surface definitions
    for (int i = 0; i < 2; ++i) {
        // u_depthScaleFactor
        ugrS.surfaceDefs_[i].graphicsCommand.UniformData[7].Data = &flC.depthScaleFactor;
    }
    LOGI("Updated depth scale factor: %f", flC.depthScaleFactor);
}

/**
 * @brief Upload new frame pixel data into the GL textures using double-buffering.
 *
 * Swaps to the alternate texture set, then uploads Y, U, V, alpha and depth
 * data from the provided VideoFrame into the corresponding GL textures.
 *
 * @param ugrC Component that provides texture format and unpack alignment info.
 * @param framePtr Pointer to a pointer to the VideoFrame containing data buffers
 *                 and stride information.
 * @param ugrS The render state which holds texture handles and current surface set.
 */
void UnlitGeometryRenderSystem::UpdateTextures(
    UnlitGeometryRenderComponent &ugrC,
    VideoFrame** framePtr,
    UnlitGeometryRenderState &ugrS) {
    // Swap the current surface set index to point to the other set.
    ugrS.currentSurfaceSet_ = (ugrS.currentSurfaceSet_ + 1) % 2;

    // UPLOAD ALL TEXTURES
    UpdateGlTexture(ugrS.textures_[ugrS.currentSurfaceSet_][TEX_Y], GL_RED,
                    (*framePtr)->textureYData.data(),
                    ugrC.texture_unpack_alignments_[TEX_Y],
                    (*framePtr)->textureYStride);
    UpdateGlTexture(ugrS.textures_[ugrS.currentSurfaceSet_][TEX_U], GL_RED,
                    (*framePtr)->textureUData.data(),
                    ugrC.texture_unpack_alignments_[TEX_U],
                    (*framePtr)->textureUStride);
    UpdateGlTexture(ugrS.textures_[ugrS.currentSurfaceSet_][TEX_V], GL_RED,
                    (*framePtr)->textureVData.data(),
                    ugrC.texture_unpack_alignments_[TEX_V],
                    (*framePtr)->textureVStride);
    UpdateGlTexture(ugrS.textures_[ugrS.currentSurfaceSet_][TEX_ALPHA], GL_RED,
                    (*framePtr)->textureAlphaData.data(),
                    ugrC.texture_unpack_alignments_[TEX_ALPHA],
                    (*framePtr)->textureAlphaStride);
    UpdateGlTexture(ugrS.textures_[ugrS.currentSurfaceSet_][TEX_DEPTH], GL_RED_INTEGER,
                    (*framePtr)->textureDepthData.data(),
                    ugrC.texture_unpack_alignments_[TEX_DEPTH],
                    (*framePtr)->textureDepthStride);
}

/**
 * @brief Updates an OpenGL texture with 8-bit data (e.g., Y, U, V, Alpha).
 *
 * Sets the `GL_UNPACK_ALIGNMENT` while the upload is performed and uploads
 * the provided byte buffer into the previously-allocated texture with
 * `glTexSubImage2D`.
 *
 * @param texture The GlTexture object to update.
 * @param format The format of the source pixel data (e.g., GL_RED).
 * @param textureData Pointer to the 8-bit texture data buffer.
 * @param unpack_alignment GL unpack alignment to use during the upload.
 * @param stride The stride (row length in bytes) of the source textureData buffer.
 *               If 0, a packed buffer is assumed.
 */
void UnlitGeometryRenderSystem::UpdateGlTexture(
       OVRFW::GlTexture texture, GLenum format,
       const uint8_t* textureData, int unpack_alignment, int stride = 0) {
    if (unpack_alignment != 4)
    {
        glPixelStorei(GL_UNPACK_ALIGNMENT, unpack_alignment);
    }

    // If a valid stride (in bytes) is provided and it's different from the packed width,
    // tell OpenGL about the source buffer's row length in pixels.
    //if (stride > 0 && stride != (texture.Width)) {
    //    glPixelStorei(GL_UNPACK_ROW_LENGTH, stride / 2);
    //}

    glBindTexture(GL_TEXTURE_2D, texture.texture);
    glTexSubImage2D(
            GL_TEXTURE_2D,
            0,
            0,
            0,
            texture.Width,
            texture.Height,
            format,
            GL_UNSIGNED_BYTE,
            textureData);

    if (unpack_alignment != 4)
    {
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    }

    // Reset the row length to its default (0) so it doesn't affect other texture uploads.
    //if (stride > 0 && stride != (texture.Width)) {
    //    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    //}
}

/**
 * @brief Updates an OpenGL texture with 16-bit data (e.g., depth).
 *
 * Temporarily sets `GL_UNPACK_ALIGNMENT` and uploads 16-bit unsigned
 * short data using `glTexSubImage2D` with `GL_UNSIGNED_SHORT`.
 *
 * @param texture The GlTexture object to update.
 * @param format The format of the source pixel data (e.g., GL_RED_INTEGER).
 * @param textureData Pointer to the 16-bit texture data buffer.
 * @param unpack_alignment GL unpack alignment to use during the upload.
 * @param stride The stride (row length in bytes) of the source textureData buffer.
 *               If 0, a packed buffer is assumed.
 */
void UnlitGeometryRenderSystem::UpdateGlTexture(
       OVRFW::GlTexture texture, GLenum format,
       const uint16_t* textureData, int unpack_alignment, int stride = 0) {
    if (unpack_alignment != 4)
    {
        glPixelStorei(GL_UNPACK_ALIGNMENT, unpack_alignment);
    }

    // If a valid stride (in bytes) is provided and it's different from the packed width,
    // tell OpenGL about the source buffer's row length in pixels.
    //if (stride > 0 && stride != (texture.Width * 2)) {
    //    glPixelStorei(GL_UNPACK_ROW_LENGTH, stride / 2);
    //}

    glBindTexture(GL_TEXTURE_2D, texture.texture);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glTexSubImage2D(
            GL_TEXTURE_2D,
            0,
            0,
            0,
            texture.Width,
            texture.Height,
            format,
            GL_UNSIGNED_SHORT,
            textureData);

    if (unpack_alignment != 4)
    {
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    }

    // Reset the row length to its default (0) so it doesn't affect other texture uploads.<
    //if (stride > 0 && stride != (texture.Width * 2)) {
    //    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    //}
}

/**
 * @brief Adds the renderer's current geometry and state to the provided surface list for rendering.
 * This function prepares the appropriate surface definition (based on the last texture update)
 * and pushes it onto the list of surfaces to be drawn in a frame.
 *
 * @param surfaceList A reference to a vector of ovrDrawSurface to which the renderer's
 *                    surface definition will be added.
 */
void UnlitGeometryRenderSystem::Render(EntityManager& ecs, std::vector<OVRFW::ovrDrawSurface>& surfaceList) {
    ecs.ForEachMulti<TransformState,
                     UnlitGeometryRenderComponent,
                     UnlitGeometryRenderState>(
            [&](EntityID e,
            TransformState& tS,
            UnlitGeometryRenderComponent& ugrC,
            UnlitGeometryRenderState& ugrS) {
        // Get a reference to the graphics command of the ready surface.
        OVRFW::ovrSurfaceDef *surfaceDefToPush = &ugrS.surfaceDefs_[ugrS.currentSurfaceSet_];

        OVRFW::ovrGraphicsCommand &gc = surfaceDefToPush->graphicsCommand;
        gc.GpuState.blendMode = ugrC.BlendMode;
        gc.GpuState.blendSrc = ugrC.BlendSrc;
        gc.GpuState.blendDst = ugrC.BlendDst;

        surfaceList.push_back(OVRFW::ovrDrawSurface(tS.modelMatrix, surfaceDefToPush));
    });
}