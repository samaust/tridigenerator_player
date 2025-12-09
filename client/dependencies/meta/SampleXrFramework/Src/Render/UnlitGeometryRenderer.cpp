/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * Licensed under the Oculus SDK License Agreement (the "License");
 * you may not use the Oculus SDK except in compliance with the License,
 * which is provided at the time of installation or download, or which
 * otherwise accompanies this software in either electronic or hard copy form.
 *
 * You may obtain a copy of the License at
 * https://developer.oculus.com/licenses/oculussdk/
 *
 * Unless required by applicable law or agreed to in writing, the Oculus SDK
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
/*******************************************************************************

Filename    :   UnlitGeometryRenderer.cpp
Content     :   Unlit rendering for geometry-based types
Based on    :   Render/GeometryRenderer.cpp, by Federico Schliemann, Mar 2021
Created     :   2025
Authors     :   Samuel Austin
Language    :   C++

*******************************************************************************/

#include <GLES3/gl3.h>

#include "GlTexture.h"
#include "UnlitGeometryRenderer.h"
#include "gl_pixel_format.h"
#include "Misc/Log.h"

using OVR::Matrix4f;
using OVR::Posef;
using OVR::Quatf;
using OVR::Vector2f;
using OVR::Vector3f;
using OVR::Vector4f;

namespace OVRFW {

    const char* UnlitGeometryVertexShaderSrc = R"glsl(
// Attributes
attribute highp vec4 Position;
attribute highp vec3 Normal;
#ifdef USE_TEXTURE
attribute highp vec2 TexCoord;
#endif

uniform usampler2D u_texDepth;
uniform highp float u_FovX_rad; // Horizontal FOV in radians (e.g., fovx_deg * PI / 180.0)
uniform highp float u_FovY_rad; // Vertical FOV in radians (calculated from aspect ratio)
uniform highp float u_depthScaleFactor;


// Outputs to fragment shader
varying lowp vec2 oTexCoord;
varying lowp vec4 oColor;

void main()
{
    // Reconstruct 16-bit depth value
    uint uz = texture(u_texDepth, TexCoord).r;

    // Convert to meters using the factor from the manifest
    float z = float(uz) / u_depthScaleFactor;

    // Calculate X and Y world coordinates using projection math.
    // TexCoord is [0, 1]. Convert to Normalized Device Coordinates [-1, 1].
    float ndc_x = TexCoord.x * 2.0 - 1.0;
    // For Y, texture coordinates often have 0 at the top. We need to flip this
    // so that +Y in screen space maps to +Y in world space.
    float ndc_y = 1.0 - TexCoord.y * 2.0;

    // The tangent of the half-FOV gives the extent of the view plane at distance 1.
    // We multiply by NDC to find the point on that plane, then scale by depth.
    float x = ndc_x * tan(u_FovX_rad) * z;
    float y = ndc_y * tan(u_FovY_rad) * z;

    //float x = ndc_x;
    //float y = ndc_y;

    // The Z coordinate in view space is negative.
    vec4 worldPosition = vec4(x, y, -z, 1.0);

    // Transform from local model space to world/view/clip space
    gl_Position = TransformVertex( worldPosition );
    oTexCoord = TexCoord;
    oColor = vec4(1,1,1,1);
}
)glsl";

    static const char* UnlitGeometryFragmentShaderSrc = R"glsl(
uniform sampler2D u_texY;
uniform sampler2D u_texU;
uniform sampler2D u_texV;
uniform sampler2D u_texAlpha;

varying lowp vec2 oTexCoord;
varying lowp vec4 oColor;

vec3 yuv_to_rgb(float y, float u, float v) {
    float c = y - 0.0625;
    float d = u - 0.5;
    float e = v - 0.5;
    float r = 1.1643 * c + 1.5958 * e;
    float g = 1.1643 * c - 0.39173 * d - 0.81290 * e;
    float b = 1.1643 * c + 2.017 * d;
    return vec3(r, g, b);
}

void main()
{
    // Get alpha value from its own texture
    float alpha = texture(u_texAlpha, oTexCoord).r;

    // Discard fragment if alpha is below a threshold
    if (alpha < 0.5) discard;

    // Get color value from YUV textures
    float y = texture(u_texY, oTexCoord).r;
    float u = texture(u_texU, oTexCoord).r;
    float v = texture(u_texV, oTexCoord).r;
    vec3 rgb = yuv_to_rgb(y, u, v);

    gl_FragColor.xyz = rgb;
    gl_FragColor.w = 1.0;
}
)glsl";


    /**
     * @brief Creates an immutable OpenGL 2D texture.
     *
     * @param internalformat The internal format of the texture (e.g., GL_R8, GL_R16UI).
     * @param pixelWidth The width of the texture in pixels.
     * @param pixelHeight The height of the texture in pixels.
     * @return A GlTexture object representing the created texture.
     */
    GlTexture UnlitGeometryRenderer::CreateGlTexture(GLenum internalformat, uint32_t pixelWidth, uint32_t pixelHeight) {
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

        return GlTexture(texId, GL_TEXTURE_2D, pixelWidth, pixelHeight);
    }


    /**
     * @brief Updates an OpenGL texture with 8-bit data (e.g., Y, U, V, Alpha).
     *
     * @param texture The GlTexture object to update.
     * @param format The format of the source pixel data (e.g., GL_RED).
     * @param textureData A pointer to the 8-bit texture data.
     * @param stride The stride (row length in bytes) of the source textureData buffer.
     *               If 0, a packed buffer is assumed.
     */
    void UnlitGeometryRenderer::UpdateGlTexture(GlTexture texture, GLenum format, const uint8_t* textureData, int unpack_alignment, int stride = 0) {
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
     * @param texture The GlTexture object to update.
     * @param format The format of the source pixel data (e.g., GL_RED_INTEGER).
     * @param textureData A pointer to the 16-bit texture data.
     * @param stride The stride (row length in bytes) of the source textureData buffer.
     *               If 0, a packed buffer is assumed.
     */
    void UnlitGeometryRenderer::UpdateGlTexture(GlTexture texture, GLenum format, const uint16_t* textureData, int unpack_alignment, int stride = 0) {
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
     * @brief Initializes the renderer, creating the GL program and geometry.
     *
     * @param d A descriptor containing the initial geometry data (vertices, indices, etc.).
     */
     void UnlitGeometryRenderer::Init(const GlGeometry::Descriptor& d) {
        /// Program
        static ovrProgramParm GeometryUniformParms[] = {
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

        /// Do we support vertex color in the geometry
        const bool hasVertexColors = (d.attribs.color.size() > 0);
        if (hasVertexColors) {
            programDefs += "#define HAS_VERTEX_COLORS 1\n";
        }
        const bool hasMultipleParts = (d.attribs.jointIndices.size() > 0);
        if (hasMultipleParts) {
            programDefs += "#define HAS_MULTIPLE_PARTS 1\n";
        }
        programDefs += "#define USE_TEXTURE 1\n";

        // Initialize BOTH surface definitions
        for (int i = 0; i < 2; ++i) {
            surfaceDefs_[i].geo = GlGeometry(d.attribs, d.indices);

            Program_ = GlProgram::Build(
                    programDefs.c_str(),
                    UnlitGeometryVertexShaderSrc,
                    programDefs.c_str(),
                    UnlitGeometryFragmentShaderSrc,
                    GeometryUniformParms,
                    sizeof(GeometryUniformParms) / sizeof(ovrProgramParm));

            /// Hook the graphics command
            ovrGraphicsCommand &gc = surfaceDefs_[i].graphicsCommand;
            gc.Program = Program_;
            /// Uniforms
            //gc.UniformData[0].Data = &gc.Textures[0]; // u_texY
            //gc.UniformData[1].Data = &gc.Textures[1]; // u_texU
            //gc.UniformData[2].Data = &gc.Textures[2]; // u_texV

            /// gpu state needs alpha blending
            gc.GpuState.depthEnable = gc.GpuState.depthMaskEnable = true;
            gc.GpuState.blendEnable = ovrGpuState::BLEND_ENABLE;
        }
    }


    /**
     * @brief Shuts down the renderer, freeing all allocated GL resources.
     * This includes textures, the GL program, and geometry buffers.
     */
    void UnlitGeometryRenderer::Shutdown() {
        for (int i = 0; i < 2; ++i) {
            FreeTexture(textures_[i][TEX_Y]);
            FreeTexture(textures_[i][TEX_U]);
            FreeTexture(textures_[i][TEX_V]);
            FreeTexture(textures_[i][TEX_ALPHA]);
            FreeTexture(textures_[i][TEX_DEPTH]);
        }
        GlProgram::Free(Program_);
        surfaceDefs_[0].geo.Free();
        surfaceDefs_[1].geo.Free();
    }

    void UnlitGeometryRenderer::Update() {
        ModelPose_.Rotation.Normalize();
        ModelMatrix_ = OVR::Matrix4f(ModelPose_) * OVR::Matrix4f::Scaling(ModelScale_);
    }

    void UnlitGeometryRenderer::UpdateGeometry(const GlGeometry::Descriptor& d) {
        surfaceDefs_[0].geo.Update(d.attribs);
        surfaceDefs_[1].geo.Update(d.attribs);
    }

    void UnlitGeometryRenderer::UpdateFov(float fovx_deg) {
        float aspect_ratio = 1.0f;
        if (textures_[0][0].Width != 0 && textures_[0][0].Height != 0) {
            aspect_ratio = (float)textures_[0][0].Height / (float)textures_[0][0].Width;
        }

        fovx_rad_ = fovx_deg * (M_PI / 180.0f) / 2.0f;

        // This is the correct formula to derive vertical FOV from horizontal FOV and aspect ratio
        fovy_rad_ = atan(tan(fovx_rad_) * aspect_ratio);

        // Set the uniform values on the renderer's graphics command
        for (int i = 0; i < 2; ++i) {
            // Assuming uniform indices 3 and 4 match the list order
            surfaceDefs_[i].graphicsCommand.UniformData[5].Data = &fovx_rad_;
            surfaceDefs_[i].graphicsCommand.UniformData[6].Data = &fovy_rad_;
        }
    }


    /**
     * @brief Updates the depth scale factor used in the vertex shader.
     * This factor is used to convert the 16-bit depth texture values into meters.
     *
     * @param factor The new depth scale factor.
     */
    void UnlitGeometryRenderer::UpdateDepthScaleFactor(float factor) {
        depthScaleFactor_ = factor;
        // Update the uniform data in both surface definitions
        for (int i = 0; i < 2; ++i) {
            // The new uniform is at index 8.
            surfaceDefs_[i].graphicsCommand.UniformData[7].Data = &depthScaleFactor_;
        }
    }


    /**
     * @brief Creates and initializes the OpenGL textures needed for rendering.
     * This includes Y, U, V, alpha, and depth textures. Two sets are created for double-buffering.
     *
     * @param textureYWidth Width of the Y (luma) texture.
     * @param textureYHeight Height of the Y (luma) texture.
     * @param textureUWidth Width of the U (chroma) texture.
     * @param textureUHeight Height of the U (chroma) texture.
     * @param textureVWidth Width of the V (chroma) texture.
     * @param textureVHeight Height of the V (chroma) texture.
     * @param textureAlphaWidth Width of the alpha texture.
     * @param textureAlphaHeight Height of the alpha texture.
     * @param textureDepthWidth Width of the depth texture.
     * @param textureDepthHeight Height of the depth texture.
     */
    void UnlitGeometryRenderer::CreateTextures(uint32_t textureYWidth, uint32_t textureYHeight,
                                               uint32_t textureUWidth, uint32_t textureUHeight,
                                               uint32_t textureVWidth, uint32_t textureVHeight,
                                               uint32_t textureAlphaWidth, uint32_t textureAlphaHeight,
                                               uint32_t textureDepthWidth, uint32_t textureDepthHeight) {
        for (int i = 0; i < 2; ++i) {
            // YUV textures
            textures_[i][TEX_Y] = CreateGlTexture(texture_internal_formats_[TEX_Y], textureYWidth, textureYHeight);
            textures_[i][TEX_U] = CreateGlTexture(texture_internal_formats_[TEX_U], textureUWidth, textureUHeight);
            textures_[i][TEX_V] = CreateGlTexture(texture_internal_formats_[TEX_V], textureVWidth, textureVHeight);

            // Alpha texture
            textures_[i][TEX_ALPHA] = CreateGlTexture(texture_internal_formats_[TEX_ALPHA], textureAlphaWidth, textureAlphaHeight);

            // Create one 16-bit textures for the 16-bit depth data
            textures_[i][TEX_DEPTH] = CreateGlTexture(texture_internal_formats_[TEX_DEPTH], textureDepthWidth, textureDepthHeight);

            // --- Assign textures to their corresponding surface def ---
            ovrGraphicsCommand &gc = surfaceDefs_[i].graphicsCommand;
            gc.Textures[TEX_Y] = textures_[i][TEX_Y];
            gc.Textures[TEX_U] = textures_[i][TEX_U];
            gc.Textures[TEX_V] = textures_[i][TEX_V];
            gc.Textures[TEX_ALPHA] = textures_[i][TEX_ALPHA];
            gc.Textures[TEX_DEPTH] = textures_[i][TEX_DEPTH];
            gc.BindUniformTextures();
        }

        // Compute bytes per row
        const uint32_t tex_Y_bpr = textureYWidth * bytesPerPixel(texture_internal_formats_[TEX_Y]);
        const uint32_t tex_U_bpr = textureUWidth * bytesPerPixel(texture_internal_formats_[TEX_U]);
        const uint32_t tex_V_bpr = textureVWidth * bytesPerPixel(texture_internal_formats_[TEX_V]);
        const uint32_t tex_alpha_bpr = textureAlphaWidth * bytesPerPixel(texture_internal_formats_[TEX_ALPHA]);
        const uint32_t tex_depth_bpr = textureDepthWidth * bytesPerPixel(texture_internal_formats_[TEX_DEPTH]);

        // Set unpack alignments based on the frame data strides
        // Another choice is to use stride values from frame directly
        texture_unpack_alignments_[TEX_Y] = computeUnpackAlignment(tex_Y_bpr);
        texture_unpack_alignments_[TEX_U] = computeUnpackAlignment(tex_U_bpr);
        texture_unpack_alignments_[TEX_V] = computeUnpackAlignment(tex_V_bpr);
        texture_unpack_alignments_[TEX_ALPHA] = computeUnpackAlignment(tex_alpha_bpr);
        texture_unpack_alignments_[TEX_DEPTH] = computeUnpackAlignment(tex_depth_bpr);

        // Start with set 0 as the one to be rendered.
        currentSurfaceSet_ = 0;
    }


    /**
     * @brief Updates the content of the OpenGL textures with new data from a VideoFrame.
     * This function implements double-buffering by swapping to the next texture set before uploading.
     * It uploads Y, U, V, alpha, and depth data to the GPU.
     *
     * @param frame A pointer to a VideoFrame struct containing the new texture data and strides.
     */
    void UnlitGeometryRenderer::UpdateTextures(const VideoFrame* frame) {
        // Swap the current surface set index to point to the other set.
        currentSurfaceSet_ = (currentSurfaceSet_ + 1) % 2;

        // UPLOAD ALL TEXTURES
        UpdateGlTexture(textures_[currentSurfaceSet_][TEX_Y], GL_RED, frame->textureYData.data(), texture_unpack_alignments_[TEX_Y], frame->textureYStride);
        UpdateGlTexture(textures_[currentSurfaceSet_][TEX_U], GL_RED, frame->textureUData.data(), texture_unpack_alignments_[TEX_U], frame->textureUStride);
        UpdateGlTexture(textures_[currentSurfaceSet_][TEX_V], GL_RED, frame->textureVData.data(), texture_unpack_alignments_[TEX_V], frame->textureVStride);
        UpdateGlTexture(textures_[currentSurfaceSet_][TEX_ALPHA], GL_RED, frame->textureAlphaData.data(), texture_unpack_alignments_[TEX_ALPHA], frame->textureAlphaStride);
        UpdateGlTexture(textures_[currentSurfaceSet_][TEX_DEPTH], GL_RED_INTEGER, frame->textureDepthData.data(), texture_unpack_alignments_[TEX_DEPTH], frame->textureDepthStride);
    }


    /**
     * @brief Adds the renderer's current geometry and state to the provided surface list for rendering.
     * This function prepares the appropriate surface definition (based on the last texture update)
     * and pushes it onto the list of surfaces to be drawn in a frame.
     *
     * @param surfaceList A reference to a vector of ovrDrawSurface to which the renderer's
     *                    surface definition will be added.
     */
    void UnlitGeometryRenderer::Render(std::vector<ovrDrawSurface>& surfaceList) {
        // Get a reference to the graphics command of the ready surface.
        ovrSurfaceDef* surfaceDefToPush = &surfaceDefs_[currentSurfaceSet_];

        ovrGraphicsCommand& gc = surfaceDefToPush->graphicsCommand;
        gc.GpuState.blendMode = BlendMode;
        gc.GpuState.blendSrc = BlendSrc;
        gc.GpuState.blendDst = BlendDst;

        surfaceList.push_back(ovrDrawSurface(ModelMatrix_, surfaceDefToPush));
    }

} // namespace OVRFW
