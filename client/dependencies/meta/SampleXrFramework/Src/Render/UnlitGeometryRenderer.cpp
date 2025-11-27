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

    Program_ = GlProgram::Build(
        programDefs.c_str(),
        UnlitGeometryVertexShaderSrc,
        programDefs.c_str(),
        UnlitGeometryFragmentShaderSrc,
        GeometryUniformParms,
        sizeof(GeometryUniformParms) / sizeof(ovrProgramParm));

    SurfaceDef_.geo = GlGeometry(d.attribs, d.indices);

    /// Hook the graphics command
    ovrGraphicsCommand& gc = SurfaceDef_.graphicsCommand;
    gc.Program = Program_;
    /// Uniforms
    gc.UniformData[0].Data = &gc.Textures[0]; // u_texY
    gc.UniformData[1].Data = &gc.Textures[1]; // u_texU
    gc.UniformData[2].Data = &gc.Textures[2]; // u_texV

    /// gpu state needs alpha blending
    gc.GpuState.depthEnable = gc.GpuState.depthMaskEnable = true;
    gc.GpuState.blendEnable = ovrGpuState::BLEND_ENABLE;
}

void UnlitGeometryRenderer::Shutdown() {
    GlProgram::Free(Program_);
    SurfaceDef_.geo.Free();
}

void UnlitGeometryRenderer::Update() {
    ModelPose_.Rotation.Normalize();
    ModelMatrix_ = OVR::Matrix4f(ModelPose_) * OVR::Matrix4f::Scaling(ModelScale_);
}

void UnlitGeometryRenderer::UpdateGeometry(const GlGeometry::Descriptor& d) {
    SurfaceDef_.geo.Update(d.attribs);
}

void UnlitGeometryRenderer::CreateTexture(
        uint32_t textureWidth,
        uint32_t textureHeight) {
    TexY_ = CreateGlTexture(textureWidth, textureHeight);
    TexU_ = CreateGlTexture(textureWidth, textureHeight);
    TexV_ = CreateGlTexture(textureWidth, textureHeight);
}

void UnlitGeometryRenderer::UpdateTexture(
        const uint8_t* textureYData,
        const uint8_t* textureUData,
        const uint8_t* textureVData,
        uint32_t textureWidth,
        uint32_t textureHeight) {
    if (TexY_.Width != static_cast<int>(textureWidth) ||
            TexY_.Height != static_cast<int>(textureHeight)) {
        ALOGE("Invalid unlit geometry texture dimensions for TexY_");
        return;
    }
    UpdateGlTexture(TexY_, textureYData);
    UpdateGlTexture(TexU_, textureUData);
    UpdateGlTexture(TexV_, textureVData);
}

void UnlitGeometryRenderer::Render(std::vector<ovrDrawSurface>& surfaceList) {
    ovrGraphicsCommand& gc = SurfaceDef_.graphicsCommand;
    gc.GpuState.blendMode = BlendMode;
    gc.GpuState.blendSrc = BlendSrc;
    gc.GpuState.blendDst = BlendDst;
    surfaceList.push_back(ovrDrawSurface(ModelMatrix_, &SurfaceDef_));
}

} // namespace OVRFW
