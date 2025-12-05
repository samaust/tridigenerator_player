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
Content     :   Simple rendering for geometry-based types
Created     :   Mar 2021
Authors     :   Federico Schliemann
Language    :   C++

*******************************************************************************/

#include <GLES3/gl3.h>

#include "Render/GlTexture.h"

#include "UnlitGeometryRenderer.h"
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

uniform sampler2D u_texY;
uniform sampler2D u_texU;
uniform sampler2D u_texV;
uniform highp float u_FovX_rad; // Horizontal FOV in radians (e.g., fovx_deg * PI / 180.0)
uniform highp float u_FovY_rad; // Vertical FOV in radians (calculated from aspect ratio)


// Outputs to fragment shader
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

vec2 yuv_to_gb(float y, float u, float v) {
    float c = y - 0.0625;
    float d = u - 0.5;
    float e = v - 0.5;
    float g = 1.1643 * c - 0.39173 * d - 0.81290 * e;
    float b = 1.1643 * c + 2.017 * d;
    return vec2(g, b);
}

void main()
{
    // Reconstruct Z value (depth)
    vec2 dataTexCoord = vec2(TexCoord.x * 0.5, TexCoord.y);
    float y_data = texture(u_texY, dataTexCoord).r;
    float u_data = texture(u_texU, dataTexCoord).r;
    float v_data = texture(u_texV, dataTexCoord).r;
    vec3 data_rgb = yuv_to_rgb(y_data, u_data, v_data);
    float highByte = data_rgb.g * 255.0;
    float lowByte = data_rgb.b * 255.0;
    float z = max((highByte * 256.0 + lowByte) / 1000.0, 0.01);

    float ndc_x = TexCoord.x * 2.0 - 1.0;
    float ndc_y = -(TexCoord.y * 2.0 - 1.0);
    float angle_x = ndc_x * (u_FovX_rad);
    float angle_y = ndc_y * (u_FovY_rad);
    float x = z * tan(angle_x);
    float y = z * tan(angle_y);

    vec4 worldPosition = vec4(x, y, -z, 1.0);

    gl_Position = TransformVertex( worldPosition );
    oTexCoord = TexCoord;
    oColor = vec4(1,1,1,1);
}
)glsl";

    static const char* UnlitGeometryFragmentShaderSrc = R"glsl(
precision lowp float;

uniform sampler2D u_texY;
uniform sampler2D u_texU;
uniform sampler2D u_texV;

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

float yv_to_r(float y, float v) {
    float c = y - 0.0625;
    float e = v - 0.5;
    float r = 1.1643 * c + 1.5958 * e;
    return r;
}

void main()
{
    vec2 dataTexCoord = vec2(oTexCoord.x * 0.5, oTexCoord.y);
    vec2 colorTexCoord = vec2(oTexCoord.x * 0.5 + 0.5, oTexCoord.y);

    float y = texture(u_texY, dataTexCoord).r;
    float v = texture(u_texV, dataTexCoord).r;
    float alpha = yv_to_r(y, v);

    y = texture(u_texY, colorTexCoord).r;
    float u = texture(u_texU, colorTexCoord).r;
    v = texture(u_texV, colorTexCoord).r;
    vec3 rgb = yuv_to_rgb(y, u, v);

    if (alpha < 0.5) discard;

    gl_FragColor.xyz = rgb;
    gl_FragColor.w = 1.0;
}
)glsl";

    GlTexture UnlitGeometryRenderer::CreateGlTexture(uint32_t pixelWidth, uint32_t pixelHeight) {
        GLuint texId;
        glGenTextures(1, &texId);
        glBindTexture(GL_TEXTURE_2D, texId);

        // sensible defaults
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        // single channel internal format for Y/U/V planes
        glTexStorage2D(GL_TEXTURE_2D, 1, GL_R8, pixelWidth, pixelHeight);

        std::vector<uint8_t> blankBytes((size_t)pixelWidth * pixelHeight);
        glTexSubImage2D(
                GL_TEXTURE_2D,
                0,
                0,
                0,
                pixelWidth,
                pixelHeight,
                GL_RED,
                GL_UNSIGNED_BYTE,
                blankBytes.data());

        return GlTexture(texId, GL_TEXTURE_2D, pixelWidth, pixelHeight);
    }

    void UnlitGeometryRenderer::UpdateGlTexture(GlTexture texture, const uint8_t* textureData) {
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glBindTexture(GL_TEXTURE_2D, texture.texture);
        glTexSubImage2D(
                GL_TEXTURE_2D,
                0,
                0,
                0,
                texture.Width,
                texture.Height,
                GL_RED,
                GL_UNSIGNED_BYTE,
                textureData);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    }

    void UnlitGeometryRenderer::Init(const GlGeometry::Descriptor& d) {
        /// Program
        static ovrProgramParm GeometryUniformParms[] = {
                {"u_texY", OVRFW::ovrProgramParmType::TEXTURE_SAMPLED},
                {"u_texU", OVRFW::ovrProgramParmType::TEXTURE_SAMPLED},
                {"u_texV", OVRFW::ovrProgramParmType::TEXTURE_SAMPLED},
                {"u_FovX_rad", OVRFW::ovrProgramParmType::FLOAT},
                {"u_FovY_rad", OVRFW::ovrProgramParmType::FLOAT},
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

    void UnlitGeometryRenderer::Shutdown() {
        FreeTexture(textures_[0][0]);
        FreeTexture(textures_[0][1]);
        FreeTexture(textures_[0][2]);
        FreeTexture(textures_[1][0]);
        FreeTexture(textures_[1][1]);
        FreeTexture(textures_[1][2]);
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

        fovx_rad = fovx_deg * (M_PI / 180.0f) / 2.0f;

        // This is the correct formula to derive vertical FOV from horizontal FOV and aspect ratio
        fovy_rad = atan(tan(fovx_rad) * aspect_ratio);

        // Set the uniform values on the renderer's graphics command
        for (int i = 0; i < 2; ++i) {
            // Assuming uniform indices 3 and 4 match the list order
            surfaceDefs_[i].graphicsCommand.UniformData[3].Data = &fovx_rad;
            surfaceDefs_[i].graphicsCommand.UniformData[4].Data = &fovy_rad;
        }
    }

    void UnlitGeometryRenderer::CreateTextures(
            uint32_t textureYWidth,
            uint32_t textureYHeight,
            uint32_t textureUWidth,
            uint32_t textureUHeight,
            uint32_t textureVWidth,
            uint32_t textureVHeight) {

        for (int i = 0; i < 2; ++i) {
            textures_[i][0] = CreateGlTexture(textureYWidth, textureYHeight); // Y
            textures_[i][1] = CreateGlTexture(textureUWidth, textureUHeight);   // U
            textures_[i][2] = CreateGlTexture(textureVWidth, textureVHeight);   // V


            // Assign textures to their corresponding surface def ---
            ovrGraphicsCommand &gc = surfaceDefs_[i].graphicsCommand;
            gc.Textures[0] = textures_[i][0];
            gc.Textures[1] = textures_[i][1];
            gc.Textures[2] = textures_[i][2];
            gc.BindUniformTextures();
        }

        // Start with set 0 as the one to be rendered.
        currentSurfaceSet_ = 0;
    }

    void UnlitGeometryRenderer::UpdateTextures(
            const uint8_t* textureYData,
            uint32_t textureYWidth,
            uint32_t textureYHeight,
            const uint8_t* textureUData,
            uint32_t textureUWidth,
            uint32_t textureUHeight,
            const uint8_t* textureVData,
            uint32_t textureVWidth,
            uint32_t textureVHeight) {

        // Swap the current surface set index to point to the other set.
        currentSurfaceSet_ = (currentSurfaceSet_ + 1) % 2;

        // Get references to the textures we will update
        GlTexture& texY = textures_[currentSurfaceSet_][0];
        GlTexture& texU = textures_[currentSurfaceSet_][1];
        GlTexture& texV = textures_[currentSurfaceSet_][2];

        if (texY.Width != static_cast<int>(textureYWidth) ||
            texY.Height != static_cast<int>(textureYHeight)) {
            ALOGE("Invalid unlit geometry texture dimensions for TexY_");
            return;
        }
        if (texU.Width != static_cast<int>(textureUWidth) ||
            texU.Height != static_cast<int>(textureUHeight)) {
            ALOGE("Invalid unlit geometry texture dimensions for TexU_");
            return;
        }
        if (texV.Width != static_cast<int>(textureVWidth) ||
            texV.Height != static_cast<int>(textureVHeight)) {
            ALOGE("Invalid unlit geometry texture dimensions for TexV_");
            return;
        }

        // Update the "back buffer" textures
        UpdateGlTexture(texY, textureYData);
        UpdateGlTexture(texU, textureUData);
        UpdateGlTexture(texV, textureVData);
    }

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
