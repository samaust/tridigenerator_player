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
 * - UpdateGl8Texture(): Handles texture 8bit data upload with stride and alignment support
 * - UpdateGl16Texture(): Handles texture 16bit data upload with stride and alignment support
 * 
 * Shader Uniforms:
 * - u_texY, u_texU, u_texV, u_texAlpha, u_texDepth: Sampler uniforms
 * - u_FovX_rad, u_FovY_rad: Field of view in radians
 * - u_depthScaleFactor: Scale factor for depth value conversion
 */
#define LOG_TAG "UnlitGeometryRenderSystem"
#include "../Core/Logging.h"

#include <algorithm>
#include <array>
#include <cstring>

#include "UnlitGeometryRenderSystem.h"

#include "Render/GeometryBuilder.h"
#include "Render/gl_pixel_format.h"

#include "../Shaders/UnlitGeometryRenderShaders.h"

#include "../Systems/TransformSystem.h"

#include "../Components/TransformComponent.h"
#include "../Components/InteractableComponent.h"
#include "../Components/MeshDetailSettings.h"

#include "../States/EnvironmentDepthState.h"
#include "../States/CameraLightEstimationState.h"
#include "../Components/CameraLightEstimationComponent.h"
#include "../States/TransformState.h"
#include "../States/FrameLoaderState.h"
#include "../Data/VipeDataset.h"
#include "../Data/ColorReference.h"

using OVR::Matrix4f;
using OVR::Posef;
using OVR::Quatf;
using OVR::Vector2f;
using OVR::Vector3f;
using OVR::Vector4f;

namespace {

const std::vector<uint16_t>* PreparedDepthPixels(
        const VideoFrame& frame,
        uint32_t expectedWidth,
        uint32_t expectedHeight) {
    const size_t expectedPixels =
        static_cast<size_t>(expectedWidth) * expectedHeight;
    if (frame.preparedDepthData.size() >= expectedPixels) {
        return &frame.preparedDepthData;
    }
    if (frame.textureDepthWidth == expectedWidth &&
        frame.textureDepthHeight == expectedHeight &&
        frame.textureDepthData.size() >= expectedPixels) {
        return &frame.textureDepthData;
    }
    return nullptr;
}

struct TextureUploadSlice {
    size_t offset = 0;
    size_t size = 0;
};

constexpr size_t AlignUploadOffset(size_t value) {
    constexpr size_t alignment = 8;
    return (value + alignment - 1) & ~(alignment - 1);
}

bool StageFrameTextureUpload(
        const VideoFrame& frame,
        const std::vector<uint16_t>& depthData,
        UnlitGeometryRenderState& renderState,
        std::array<TextureUploadSlice, 5>& slices) {
    const std::array<const void*, 5> sources{
        frame.textureYData.data(),
        frame.textureUData.data(),
        frame.textureVData.data(),
        frame.textureAlphaData.data(),
        depthData.data()};
    const std::array<size_t, 5> availableBytes{
        frame.textureYData.size(),
        frame.textureUData.size(),
        frame.textureVData.size(),
        frame.textureAlphaData.size(),
        depthData.size() * sizeof(uint16_t)};
    const std::array<size_t, 5> requiredBytes{
        static_cast<size_t>(frame.textureYWidth) * frame.textureYHeight,
        static_cast<size_t>(frame.textureUWidth) * frame.textureUHeight,
        static_cast<size_t>(frame.textureVWidth) * frame.textureVHeight,
        static_cast<size_t>(frame.textureAlphaWidth) * frame.textureAlphaHeight,
        static_cast<size_t>(renderState.meshWidth_) *
            renderState.meshHeight_ * sizeof(uint16_t)};

    size_t totalBytes = 0;
    for (size_t plane = 0; plane < slices.size(); ++plane) {
        if (availableBytes[plane] < requiredBytes[plane]) return false;
        totalBytes = AlignUploadOffset(totalBytes);
        slices[plane] = {totalBytes, requiredBytes[plane]};
        totalBytes += requiredBytes[plane];
    }
    if (totalBytes == 0) return false;

    if (renderState.uploadPbos_[0] == 0) {
        glGenBuffers(
            static_cast<GLsizei>(renderState.uploadPbos_.size()),
            renderState.uploadPbos_.data());
        for (unsigned pbo : renderState.uploadPbos_) {
            if (pbo == 0) {
                glDeleteBuffers(
                    static_cast<GLsizei>(renderState.uploadPbos_.size()),
                    renderState.uploadPbos_.data());
                renderState.uploadPbos_.fill(0);
                return false;
            }
        }
        LOGI(
            "Created %zu-buffer asynchronous texture upload ring",
            renderState.uploadPbos_.size());
    }

    const unsigned pbo =
        renderState.uploadPbos_[renderState.nextUploadPbo_];
    renderState.nextUploadPbo_ =
        (renderState.nextUploadPbo_ + 1) %
        renderState.uploadPbos_.size();
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, pbo);
    glBufferData(
        GL_PIXEL_UNPACK_BUFFER,
        static_cast<GLsizeiptr>(totalBytes),
        nullptr,
        GL_STREAM_DRAW);
    void* mapped = glMapBufferRange(
        GL_PIXEL_UNPACK_BUFFER,
        0,
        static_cast<GLsizeiptr>(totalBytes),
        GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT);
    if (mapped == nullptr) {
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
        return false;
    }
    auto* destination = static_cast<uint8_t*>(mapped);
    for (size_t plane = 0; plane < slices.size(); ++plane) {
        std::memcpy(
            destination + slices[plane].offset,
            sources[plane],
            slices[plane].size);
    }
    if (glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER) != GL_TRUE) {
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
        return false;
    }
    return true;
}

} // namespace

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
bool UnlitGeometryRenderSystem::Init(EntityManager& ecs, int meshDetailDivisor) {
    const int divisor = MeshDetailControl::SanitizeDivisor(meshDetailDivisor);
    ecs.ForEachMulti<UnlitGeometryRenderComponent, UnlitGeometryRenderState, FrameLoaderComponent>(
            [&](EntityID e,
                     UnlitGeometryRenderComponent& ugrC,
                     UnlitGeometryRenderState& ugrS,
                     FrameLoaderComponent& flC) {
        // Create initial plane geometry and renderer
        const int meshWidth = MeshDetailControl::ReducedDimension(flC.width, divisor);
        const int meshHeight = MeshDetailControl::ReducedDimension(flC.height, divisor);
        const int tessX = meshWidth - 1;
        const int tessY = meshHeight - 1;
        ugrS.meshDetailDivisor_ = divisor;
        ugrS.meshWidth_ = static_cast<uint32_t>(meshWidth);
        ugrS.meshHeight_ = static_cast<uint32_t>(meshHeight);
        flC.meshDetailDivisor.store(divisor, std::memory_order_release);
        if (flC.width <= 1 || flC.height <= 1) {
            LOGW("Using fallback quad subdivisions (width=%d height=%d)", flC.width, flC.height);
        }
        LOGI(
            "Creating mesh detail %s: source=%dx%d mesh=%dx%d vertices=%zu",
            MeshDetailControl::DisplayName(divisor),
            flC.width,
            flC.height,
            meshWidth,
            meshHeight,
            MeshDetailControl::VertexCount(flC.width, flC.height, divisor));
        auto planeDescriptor = OVRFW::BuildTesselatedQuadDescriptor(
                static_cast<OVRFW::TriangleIndex>(tessX),
                static_cast<OVRFW::TriangleIndex>(tessY),
                false,
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
                {"u_environmentDepthTexture", OVRFW::ovrProgramParmType::TEXTURE_SAMPLED},
                {"u_intrinsics", OVRFW::ovrProgramParmType::FLOAT_VECTOR4},
                {"u_imageSize", OVRFW::ovrProgramParmType::FLOAT_VECTOR2},
                {"u_depthScaleFactor", OVRFW::ovrProgramParmType::FLOAT},
                {"u_depthViewMatrix", OVRFW::ovrProgramParmType::FLOAT_MATRIX4},
                {"u_depthProjectionMatrix", OVRFW::ovrProgramParmType::FLOAT_MATRIX4},
                {"u_hasEnvironmentDepth", OVRFW::ovrProgramParmType::INT},
                {"u_occlusionParams", OVRFW::ovrProgramParmType::FLOAT_VECTOR4},
                {"u_environmentDepthTexelSize", OVRFW::ovrProgramParmType::FLOAT_VECTOR2},
                {"u_lightField", OVRFW::ovrProgramParmType::TEXTURE_SAMPLED},
                {"u_lightParams", OVRFW::ovrProgramParmType::FLOAT_MATRIX4},
                {"u_datasetColorReference", OVRFW::ovrProgramParmType::TEXTURE_SAMPLED},
                {"u_matchingLimits", OVRFW::ovrProgramParmType::FLOAT_VECTOR4},
                {"u_maskVisibility[0]", OVRFW::ovrProgramParmType::INT},
        };

        std::string programDefsLimited;
        std::string programDefsFullRange = "#define YUV_FULL_RANGE 1\n";

        ugrS.ProgramLimited_ = OVRFW::GlProgram::Build(
                programDefsLimited.c_str(),
                UnlitGeometryVertexShaderSrc,
                programDefsLimited.c_str(),
                UnlitGeometryFragmentShaderSrc,
                GeometryUniformParms,
                sizeof(GeometryUniformParms) / sizeof(OVRFW::ovrProgramParm));

        ugrS.ProgramFullRange_ = OVRFW::GlProgram::Build(
                programDefsFullRange.c_str(),
                UnlitGeometryVertexShaderSrc,
                programDefsFullRange.c_str(),
                UnlitGeometryFragmentShaderSrc,
                GeometryUniformParms,
                sizeof(GeometryUniformParms) / sizeof(OVRFW::ovrProgramParm));

        // Initialize BOTH surface definitions
        for (int i = 0; i < 2; ++i) {
            ugrS.surfaceDefs_[i].geo = OVRFW::GlGeometry(d.attribs, d.indices);

            /// Hook the graphics command
            OVRFW::ovrGraphicsCommand &gc = ugrS.surfaceDefs_[i].graphicsCommand;
            gc.Program = ugrS.ProgramLimited_;
            gc.BindUniformTextures();
            gc.UniformData[18].Data = ugrC.maskVisibility_.ShaderValues();
            gc.UniformData[18].Count = 256;

            /// gpu state needs alpha blending
            gc.GpuState.depthEnable = gc.GpuState.depthMaskEnable = true;
            gc.GpuState.blendEnable = OVRFW::ovrGpuState::BLEND_ENABLE;
            gc.GpuState.cullEnable = true;
        }
    });
    return true;
}

bool UnlitGeometryRenderSystem::RebuildGeometry(
        EntityManager& ecs, int meshDetailDivisor) {
    const int divisor = MeshDetailControl::SanitizeDivisor(meshDetailDivisor);
    bool rebuilt = false;
    ecs.ForEachMulti<
        UnlitGeometryRenderState,
        FrameLoaderComponent>(
        [&](EntityID,
            UnlitGeometryRenderState& renderState,
            FrameLoaderComponent& loader) {
            const int meshWidth =
                MeshDetailControl::ReducedDimension(loader.width, divisor);
            const int meshHeight =
                MeshDetailControl::ReducedDimension(loader.height, divisor);
            auto planeDescriptor = OVRFW::BuildTesselatedQuadDescriptor(
                static_cast<OVRFW::TriangleIndex>(meshWidth - 1),
                static_cast<OVRFW::TriangleIndex>(meshHeight - 1),
                false,
                false);
            OVRFW::GeometryBuilder planeGeometry;
            planeGeometry.Add(
                planeDescriptor,
                OVRFW::GeometryBuilder::kInvalidIndex,
                OVR::Vector4f(1.0f, 0.0f, 0.0f, 1.0f));
            auto descriptor = planeGeometry.ToGeometryDescriptor();
            std::array<OVRFW::GlGeometry, 2> replacements;
            for (int surface = 0; surface < 2; ++surface) {
                replacements[surface] = OVRFW::GlGeometry(
                    descriptor.attribs, descriptor.indices);
                if (replacements[surface].vertexArrayObject == 0) {
                    LOGE(
                        "Failed to rebuild mesh detail %s for surface %d",
                        MeshDetailControl::DisplayName(divisor),
                        surface);
                    for (auto& replacement : replacements) replacement.Free();
                    return;
                }
            }
            renderState.meshDetailDivisor_ = divisor;
            renderState.meshWidth_ = static_cast<uint32_t>(meshWidth);
            renderState.meshHeight_ = static_cast<uint32_t>(meshHeight);
            for (int surface = 0; surface < 2; ++surface) {
                renderState.surfaceDefs_[surface].geo.Free();
                renderState.surfaceDefs_[surface].geo = replacements[surface];
            }
            loader.meshDetailDivisor.store(divisor, std::memory_order_release);
            LOGI(
                "Rebuilt mesh detail %s: source=%dx%d mesh/depth=%dx%d vertices=%zu",
                MeshDetailControl::DisplayName(divisor),
                loader.width,
                loader.height,
                meshWidth,
                meshHeight,
                MeshDetailControl::VertexCount(loader.width, loader.height, divisor));
            rebuilt = true;
        });
    return rebuilt;
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
            for (int texture = 0; texture < TEXTURE_SLOT_MAX; ++texture) {
                ugrS.textures_[i][texture] = {};
            }
            ugrS.surfaceDefs_[i].geo.Free();
            ugrS.surfaceDefs_[i] = {};
        }
        OVRFW::GlProgram::Free(ugrS.ProgramLimited_);
        OVRFW::GlProgram::Free(ugrS.ProgramFullRange_);
        OVRFW::FreeTexture(ugrS.datasetReferenceTexture_);
        glDeleteBuffers(
            static_cast<GLsizei>(ugrS.uploadPbos_.size()),
            ugrS.uploadPbos_.data());
        ugrS.uploadPbos_.fill(0);
        ugrS.nextUploadPbo_ = 0;
        ugrS.pboFailureLogged_ = false;
        ugrS.ProgramLimited_ = {};
        ugrS.ProgramFullRange_ = {};
        ugrS.currentSurfaceSet_ = 0;
        ugrS.depthTextureReady_ = false;
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
    EnvironmentDepthState* environmentDepthState = nullptr;
    CameraLightEstimationState* lightState = nullptr;
    CameraLightEstimationComponent* lightComponent = nullptr;
    ecs.ForEach<EnvironmentDepthState>([&](EntityID, EnvironmentDepthState& edS) {
        if (environmentDepthState == nullptr) {
            environmentDepthState = &edS;
        }
    });
    ecs.ForEachMulti<CameraLightEstimationComponent, CameraLightEstimationState>(
        [&](EntityID, CameraLightEstimationComponent& component, CameraLightEstimationState& state) {
            lightComponent = &component; lightState = &state;
        });

    ecs.ForEachMulti<TransformComponent,
                     TransformState,
                     FrameLoaderComponent,
                     FrameLoaderState,
                     InteractableComponent,
                     UnlitGeometryRenderComponent,
                     UnlitGeometryRenderState>(
        [&](EntityID e,
                 TransformComponent &tC,
                 TransformState &tS,
                 FrameLoaderComponent &flC,
                 FrameLoaderState &flS,
                 InteractableComponent& interactable,
                 UnlitGeometryRenderComponent &ugrC,
                 UnlitGeometryRenderState &ugrS) {
        // ViPE geometry is expressed relative to the capture camera. On Android,
        // align that camera with the headset once so the mesh is not left at the
        // stage origin (usually floor level and therefore fully depth-occluded).
        if (!ugrC.poseInitialized) {
            if (ugrC.poseParent == "HeadPose") {
                OVR::Posef pose = in.HeadPose;
                pose.Translation = in.HeadPose.Translate(ugrC.poseTranslationOffset);
                TransformSystem::SetPose(tC, tS, pose);
                LOGI(
                    "Placed dataset at initial head pose (%f, %f, %f)",
                    pose.Translation.x,
                    pose.Translation.y,
                    pose.Translation.z);
            }
            ugrC.poseInitialized = true;
        }

        UpdateEnvironmentDepthUniforms(ugrC, ugrS, environmentDepthState);
        if (ugrS.datasetReferenceSequence_ != flC.dataset.sequence ||
            ugrS.datasetReferenceSchemaVersion_ != flC.dataset.schemaVersion) {
            OVRFW::FreeTexture(ugrS.datasetReferenceTexture_);
            const ColorReferenceLookup lookup = BuildColorReferenceLookup(flC.dataset);
            glGenTextures(1, &ugrS.datasetReferenceTexture_.texture);
            ugrS.datasetReferenceTexture_.target = GL_TEXTURE_2D;
            ugrS.datasetReferenceTexture_.Width = 256;
            ugrS.datasetReferenceTexture_.Height = 1;
            glBindTexture(GL_TEXTURE_2D, ugrS.datasetReferenceTexture_.texture);
            glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA16F, 256, 1);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 256, 1, GL_RGBA, GL_FLOAT, lookup.data());
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            ugrS.datasetReferenceSequence_ = flC.dataset.sequence;
            ugrS.datasetReferenceSchemaVersion_ = flC.dataset.schemaVersion;
            LOGI("Dataset %s color reference: schema=%d masks=%zu",
                flC.dataset.sequence.c_str(), flC.dataset.schemaVersion,
                flC.dataset.colorReferences.masks.size());
        }
        const OVR::Vector4f global = lightState ? lightState->globalLight : OVR::Vector4f(1.0f);
        const OVR::Vector3f minimum = lightState ? lightState->gridMinimum : OVR::Vector3f(0.0f);
        const OVR::Vector3f inverseExtent = lightState ? OVR::Vector3f(
            1.0f/lightState->gridExtent.x, 1.0f/lightState->gridExtent.y, 1.0f/lightState->gridExtent.z) : OVR::Vector3f(0.0f);
        ugrS.lightParams_ = OVR::Matrix4f(
            global.x, global.y, global.z, global.w,
            minimum.x, minimum.y, minimum.z, lightState ? static_cast<float>(lightState->tier) : 0.0f,
            inverseExtent.x, inverseExtent.y, inverseExtent.z, lightState ? lightState->tierBlend : 0.0f,
            lightComponent ? lightComponent->matchingStrength : 0.0f, 0.0f, 0.0f, 0.0f);
        ugrS.matchingLimits_ = lightComponent ? OVR::Vector4f(
            lightComponent->minTint, lightComponent->maxTint,
            lightComponent->minExposure, lightComponent->maxExposure) :
            OVR::Vector4f(1.0f, 1.0f, 1.0f, 1.0f);
        for (int surface=0; surface<2; ++surface) {
            auto& gc = ugrS.surfaceDefs_[surface].graphicsCommand;
            gc.UniformData[15].Data = &ugrS.lightParams_;
            gc.UniformData[17].Data = &ugrS.matchingLimits_;
            gc.Textures[TEX_DATASET_REFERENCE] = ugrS.datasetReferenceTexture_;
            if (lightState && lightState->texturesReady) {
                gc.Textures[TEX_LIGHT_FIELD] = OVRFW::GlTexture(
                    lightState->lightFieldTexture, GL_TEXTURE_3D,
                    CameraLightEstimationState::GridWidth, CameraLightEstimationState::GridHeight);
            } else {
                gc.Textures[TEX_LIGHT_FIELD] = {};
            }
        }

        // Create color, alpha, depth textures if not already created
        if (!TexturesCreated(ugrS) && flS.framePtr != nullptr) {
            LOGI("Creating textures");
            CreateTextures(
                flS.framePtr,
                ugrC,
                ugrS);
            UpdateDepthScaleFactor(flC, ugrS);
        }

        if (flS.frameReady.load(std::memory_order_acquire)) {
            //LOGI("Update textures with new frame");
            // A new frame is available, so update textures and matrices.
            const VideoFrame& frame = **flS.framePtr;
            if (frame.preparedDepthWidth == ugrS.meshWidth_ &&
                frame.preparedDepthHeight == ugrS.meshHeight_ &&
                PreparedDepthPixels(
                    frame, ugrS.meshWidth_, ugrS.meshHeight_) != nullptr) {
                if ((ugrS.textures_[0][TEX_DEPTH].Width !=
                        static_cast<int>(ugrS.meshWidth_) ||
                     ugrS.textures_[0][TEX_DEPTH].Height !=
                        static_cast<int>(ugrS.meshHeight_)) &&
                    !RecreateDepthTextures(ugrC, ugrS)) {
                    LOGE(
                        "Failed to create %dx%d depth textures",
                        ugrS.meshWidth_,
                        ugrS.meshHeight_);
                    flS.frameReady.store(false, std::memory_order_relaxed);
                    return;
                }
                UpdateFrameGeometry(flC, frame, tC, tS, ugrS, interactable);
                UpdateTextures(ugrC, flS.framePtr, ugrS);
                ugrS.depthTextureReady_ = true;
            } else {
                LOGW(
                    "Skipping frame %d prepared at %dx%d; waiting for mesh depth %dx%d",
                    frame.frameIndex,
                    frame.preparedDepthWidth,
                    frame.preparedDepthHeight,
                    ugrS.meshWidth_,
                    ugrS.meshHeight_);
            }

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
    LOGI(
        "  Depth source: %d x %d; mesh depth: %d x %d",
        (*framePtr)->textureDepthWidth,
        (*framePtr)->textureDepthHeight,
        ugrS.meshWidth_,
        ugrS.meshHeight_);

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
        glBindTexture(GL_TEXTURE_2D, ugrS.textures_[i][TEX_ALPHA].texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

        // Create one 16-bit textures for the 16-bit depth data
        ugrS.textures_[i][TEX_DEPTH] = CreateGlTexture(
            ugrC.texture_internal_formats_[TEX_DEPTH],
            ugrS.meshWidth_,
            ugrS.meshHeight_);

        // --- Assign textures to their corresponding surface def ---
        OVRFW::ovrGraphicsCommand &gc = ugrS.surfaceDefs_[i].graphicsCommand;
        gc.Textures[TEX_Y] = ugrS.textures_[i][TEX_Y];
        gc.Textures[TEX_U] = ugrS.textures_[i][TEX_U];
        gc.Textures[TEX_V] = ugrS.textures_[i][TEX_V];
        gc.Textures[TEX_ALPHA] = ugrS.textures_[i][TEX_ALPHA];
        gc.Textures[TEX_DEPTH] = ugrS.textures_[i][TEX_DEPTH];
        gc.BindUniformTextures();
    }

    // Choose shader program based on decoded color range.
    ugrS.useFullRangeYuv_ = (*framePtr)->yuvFullRange ? 1 : 0;
    for (int i = 0; i < 2; ++i) {
        OVRFW::ovrGraphicsCommand& gc = ugrS.surfaceDefs_[i].graphicsCommand;
        gc.Program = (ugrS.useFullRangeYuv_ != 0) ? ugrS.ProgramFullRange_ : ugrS.ProgramLimited_;
        gc.BindUniformTextures();
    }

    // Compute bytes per row
    const uint32_t tex_Y_bpr = (*framePtr)->textureYWidth * bytesPerPixel(ugrC.texture_internal_formats_[TEX_Y]);
    const uint32_t tex_U_bpr = (*framePtr)->textureUWidth * bytesPerPixel(ugrC.texture_internal_formats_[TEX_U]);
    const uint32_t tex_V_bpr = (*framePtr)->textureVWidth * bytesPerPixel(ugrC.texture_internal_formats_[TEX_V]);
    const uint32_t tex_alpha_bpr = (*framePtr)->textureAlphaWidth * bytesPerPixel(ugrC.texture_internal_formats_[TEX_ALPHA]);
    const uint32_t tex_depth_bpr =
        ugrS.meshWidth_ * bytesPerPixel(ugrC.texture_internal_formats_[TEX_DEPTH]);

    // Set unpack alignments based on the frame data strides
    // Another choice is to use stride values from frame directly
    ugrC.texture_unpack_alignments_[TEX_Y] = computeUnpackAlignment(tex_Y_bpr);
    ugrC.texture_unpack_alignments_[TEX_U] = computeUnpackAlignment(tex_U_bpr);
    ugrC.texture_unpack_alignments_[TEX_V] = computeUnpackAlignment(tex_V_bpr);
    ugrC.texture_unpack_alignments_[TEX_ALPHA] = computeUnpackAlignment(tex_alpha_bpr);
    ugrC.texture_unpack_alignments_[TEX_DEPTH] = computeUnpackAlignment(tex_depth_bpr);

    // Start with set 0 as the one to be rendered.
    ugrS.currentSurfaceSet_ = 0;
    ugrS.depthTextureReady_ = false;
}

bool UnlitGeometryRenderSystem::RecreateDepthTextures(
        UnlitGeometryRenderComponent& ugrC,
        UnlitGeometryRenderState& ugrS) {
    std::array<OVRFW::GlTexture, 2> replacements;
    for (int surface = 0; surface < 2; ++surface) {
        replacements[surface] = CreateGlTexture(
            ugrC.texture_internal_formats_[TEX_DEPTH],
            ugrS.meshWidth_,
            ugrS.meshHeight_);
        if (replacements[surface].texture == 0) {
            for (auto& replacement : replacements) {
                OVRFW::FreeTexture(replacement);
            }
            return false;
        }
    }

    const uint32_t bytesPerRow =
        ugrS.meshWidth_ * bytesPerPixel(ugrC.texture_internal_formats_[TEX_DEPTH]);
    ugrC.texture_unpack_alignments_[TEX_DEPTH] =
        computeUnpackAlignment(bytesPerRow);
    for (int surface = 0; surface < 2; ++surface) {
        OVRFW::FreeTexture(ugrS.textures_[surface][TEX_DEPTH]);
        ugrS.textures_[surface][TEX_DEPTH] = replacements[surface];
        auto& command = ugrS.surfaceDefs_[surface].graphicsCommand;
        command.Textures[TEX_DEPTH] = ugrS.textures_[surface][TEX_DEPTH];
        command.BindUniformTextures();
    }
    ugrS.depthTextureReady_ = false;
    return true;
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

    if (internalformat == GL_R8UI || internalformat == GL_R16UI) {
        // Integer mask and depth values must not be interpolated.
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
void UnlitGeometryRenderSystem::UpdateFrameGeometry(
    const FrameLoaderComponent& flC,
    const VideoFrame& frame,
    TransformComponent& transform,
    TransformState& transformState,
    UnlitGeometryRenderState& renderState,
    InteractableComponent& interactable) {
    if (frame.frameIndex < 0 || frame.frameIndex >= static_cast<int>(flC.dataset.frames.size())) {
        LOGE("Decoded frame index %d is outside manifest metadata", frame.frameIndex);
        return;
    }
    const VipeFrameMetadata& metadata = flC.dataset.frames[frame.frameIndex];
    const std::array<float, 16> relative = OrientedRelativeOpenGlCameraPose(
        metadata.cameraToWorld,
        flC.dataset.frames.front().cameraToWorld,
        flC.dataset.orientationOffsetDegrees);
    const OVR::Matrix4f pose(
        relative[0], relative[1], relative[2], relative[3],
        relative[4], relative[5], relative[6], relative[7],
        relative[8], relative[9], relative[10], relative[11],
        relative[12], relative[13], relative[14], relative[15]);
    transformState.animationMatrix = pose;
    TransformSystem::Refresh(transform, transformState);
    renderState.intrinsics_ = OVR::Vector4f(
        metadata.intrinsics[0], metadata.intrinsics[1],
        metadata.intrinsics[2], metadata.intrinsics[3]);
    renderState.imageSize_ = OVR::Vector2f(
        static_cast<float>(flC.width), static_cast<float>(flC.height));

    interactable.boundsValid = frame.preparedDepthBoundsValid;
    if (interactable.boundsValid) {
        interactable.localBoundsMin = OVR::Vector3f(
            frame.preparedDepthBoundsMinimum[0],
            frame.preparedDepthBoundsMinimum[1],
            frame.preparedDepthBoundsMinimum[2]);
        interactable.localBoundsMax = OVR::Vector3f(
            frame.preparedDepthBoundsMaximum[0],
            frame.preparedDepthBoundsMaximum[1],
            frame.preparedDepthBoundsMaximum[2]);
    }
    for (int i = 0; i < 2; ++i) {
        renderState.surfaceDefs_[i].graphicsCommand.UniformData[6].Data = &renderState.intrinsics_;
        renderState.surfaceDefs_[i].graphicsCommand.UniformData[7].Data = &renderState.imageSize_;
    }
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
        ugrS.surfaceDefs_[i].graphicsCommand.UniformData[8].Data = &flC.depthScaleFactor;
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
    const std::vector<uint16_t>* depthData = PreparedDepthPixels(
        **framePtr, ugrS.meshWidth_, ugrS.meshHeight_);
    if (depthData == nullptr) {
        LOGE("UpdateTextures called without prepared depth pixels");
        return;
    }
    const int desiredFullRange = (*framePtr)->yuvFullRange ? 1 : 0;
    if (desiredFullRange != ugrS.useFullRangeYuv_) {
        ugrS.useFullRangeYuv_ = desiredFullRange;
        for (int i = 0; i < 2; ++i) {
            OVRFW::ovrGraphicsCommand& gc = ugrS.surfaceDefs_[i].graphicsCommand;
            gc.Program = (ugrS.useFullRangeYuv_ != 0) ? ugrS.ProgramFullRange_ : ugrS.ProgramLimited_;
            gc.BindUniformTextures();
        }
    }

    // Swap the current surface set index to point to the other set.
    ugrS.currentSurfaceSet_ = (ugrS.currentSurfaceSet_ + 1) % 2;

    std::array<TextureUploadSlice, 5> slices;
    const bool staged = StageFrameTextureUpload(
        **framePtr, *depthData, ugrS, slices);
    if (!staged && !ugrS.pboFailureLogged_) {
        LOGW("PBO texture staging failed; using direct uploads");
        ugrS.pboFailureLogged_ = true;
    }
    const auto uploadAddress = [&](size_t plane, const void* direct) {
        return staged
            ? reinterpret_cast<const void*>(
                static_cast<uintptr_t>(slices[plane].offset))
            : direct;
    };

    UpdateGl8Texture(
        ugrS.textures_[ugrS.currentSurfaceSet_][TEX_Y],
        GL_RED,
        uploadAddress(0, (*framePtr)->textureYData.data()),
        ugrC.texture_unpack_alignments_[TEX_Y],
        (*framePtr)->textureYStride);
    UpdateGl8Texture(
        ugrS.textures_[ugrS.currentSurfaceSet_][TEX_U],
        GL_RED,
        uploadAddress(1, (*framePtr)->textureUData.data()),
        ugrC.texture_unpack_alignments_[TEX_U],
        (*framePtr)->textureUStride);
    UpdateGl8Texture(
        ugrS.textures_[ugrS.currentSurfaceSet_][TEX_V],
        GL_RED,
        uploadAddress(2, (*framePtr)->textureVData.data()),
        ugrC.texture_unpack_alignments_[TEX_V],
        (*framePtr)->textureVStride);
    UpdateGl8Texture(
        ugrS.textures_[ugrS.currentSurfaceSet_][TEX_ALPHA],
        GL_RED_INTEGER,
        uploadAddress(3, (*framePtr)->textureAlphaData.data()),
        ugrC.texture_unpack_alignments_[TEX_ALPHA],
        (*framePtr)->textureAlphaStride);
    UpdateGl16Texture(
        ugrS.textures_[ugrS.currentSurfaceSet_][TEX_DEPTH],
        GL_RED_INTEGER,
        uploadAddress(4, depthData->data()),
        ugrC.texture_unpack_alignments_[TEX_DEPTH],
        0);
    if (staged) {
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    }
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
void UnlitGeometryRenderSystem::UpdateGl8Texture(
       OVRFW::GlTexture texture, GLenum format,
       const void* textureData, int unpack_alignment, int stride = 0) {
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
void UnlitGeometryRenderSystem::UpdateGl16Texture(
       OVRFW::GlTexture texture, GLenum format,
       const void* textureData, int unpack_alignment, int stride = 0) {
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

    // Unnecessary, done at texture creation time
    //glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    //glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    //glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    //glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

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

void UnlitGeometryRenderSystem::UpdateEnvironmentDepthUniforms(
        UnlitGeometryRenderComponent& ugrC,
        UnlitGeometryRenderState& ugrS,
        EnvironmentDepthState* environmentDepthState) {
    const bool hasDepth = environmentDepthState != nullptr &&
            environmentDepthState->IsInitialized &&
            environmentDepthState->HasDepth &&
            environmentDepthState->Image.swapchainIndex < environmentDepthState->SwapchainTextures.size();

    ugrS.hasEnvironmentDepth_ = hasDepth ? 1 : 0;

    for (int i = 0; i < 2; ++i) {
        OVRFW::ovrGraphicsCommand& gc = ugrS.surfaceDefs_[i].graphicsCommand;
        gc.UniformData[11].Data = &ugrS.hasEnvironmentDepth_;
        ugrS.occlusionParams_ = OVR::Vector4f(
            static_cast<float>(ugrC.softOcclusion_), ugrC.occlusionSoftness_, ugrC.occlusionDepthBias_, 0.0f);
        gc.UniformData[12].Data = &ugrS.occlusionParams_;
        gc.UniformData[13].Data = &ugrS.environmentDepthTexelSize_;

        if (hasDepth) {
            gc.UniformData[9].Data = environmentDepthState->DepthViewMatrices;
            gc.UniformData[9].Count = EnvironmentDepthState::kNumEyes;
            gc.UniformData[10].Data = environmentDepthState->DepthProjectionMatrices;
            gc.UniformData[10].Count = EnvironmentDepthState::kNumEyes;
            gc.Textures[TEX_ENV_DEPTH] =
                    environmentDepthState->SwapchainTextures[environmentDepthState->Image.swapchainIndex];
            if (environmentDepthState->Width > 0 && environmentDepthState->Height > 0) {
                ugrS.environmentDepthTexelSize_.x =
                    1.0f / static_cast<float>(environmentDepthState->Width);
                ugrS.environmentDepthTexelSize_.y =
                    1.0f / static_cast<float>(environmentDepthState->Height);
            } else {
                ugrS.environmentDepthTexelSize_ = OVR::Vector2f(0.0f, 0.0f);
            }
        } else {
            gc.UniformData[9].Data = nullptr;
            gc.UniformData[10].Data = nullptr;
            gc.Textures[TEX_ENV_DEPTH] = {};
            ugrS.environmentDepthTexelSize_ = OVR::Vector2f(0.0f, 0.0f);
        }
    }
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
        if (!ugrS.depthTextureReady_) return;
        OVRFW::ovrSurfaceDef *surfaceDefToPush = &ugrS.surfaceDefs_[ugrS.currentSurfaceSet_];

        OVRFW::ovrGraphicsCommand &gc = surfaceDefToPush->graphicsCommand;
        gc.GpuState.blendEnable = OVRFW::ovrGpuState::BLEND_ENABLE;
        gc.GpuState.blendMode = ugrC.BlendMode;
        gc.GpuState.blendSrc = ugrC.BlendSrc;
        gc.GpuState.blendDst = ugrC.BlendDst;

        surfaceList.push_back(OVRFW::ovrDrawSurface(tS.modelMatrix, surfaceDefToPush));
    });
}
