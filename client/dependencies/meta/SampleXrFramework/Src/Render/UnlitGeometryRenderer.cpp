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
#ifdef USE_COLOR
attribute highp vec3 Tangent;  // not used
attribute highp vec3 Binormal; // not used
attribute lowp vec4 VertexColor;
#endif

#ifdef USE_TEXTURE
attribute highp vec2 TexCoord;
attribute highp vec4 JointIndices;
attribute highp vec4 JointWeights;
#endif

// Outputs to fragment shader
varying lowp vec3 oEye;
varying lowp vec3 oNormal;
varying lowp vec2 oTexCoord;
varying lowp vec4 oColor;

void main()
{
    gl_Position = TransformVertex( Position );
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

void main()
{
    float y = texture(u_texY, oTexCoord).r;
    float u = texture(u_texU, oTexCoord).r;
    float v = texture(u_texV, oTexCoord).r;
    vec3 rgb = yuv_to_rgb(y, u, v);
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
