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
    attribute highp vec4 Position;

#ifdef HAS_VERTEX_COLORS
    attribute lowp vec4 VertexColor;
    varying lowp vec4 oColor;
#endif /// HAS_VERTEX_COLORS

    void main()
    {
        gl_Position = TransformVertex( Position );
#ifdef HAS_VERTEX_COLORS
        oColor = VertexColor;
#endif /// HAS_VERTEX_COLORS
    }
)glsl";

static const char* UnlitGeometryFragmentShaderSrc = R"glsl(
    precision lowp float;

#ifdef HAS_VERTEX_COLORS
    varying lowp vec4 oColor;
#endif /// HAS_VERTEX_COLORS

    void main()
    {
        if (oColor.a < 0.01)
            discard;

        gl_FragColor = oColor;
    }
)glsl";

void UnlitGeometryRenderer::Init(const GlGeometry::Descriptor& d) {
    /// Program
    static ovrProgramParm GeometryUniformParms[] = {
    };

    std::string programDefs;

    /// Do we support vertex color in the goemetyr
    const bool hasVertexColors = (d.attribs.color.size() > 0);
    if (hasVertexColors) {
        programDefs += "#define HAS_VERTEX_COLORS 1\n";
    }
    const bool hasMultipleParts = (d.attribs.jointIndices.size() > 0);
    if (hasMultipleParts) {
        programDefs += "#define HAS_MULTIPLE_PARTS 1\n";
    }

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

void UnlitGeometryRenderer::Render(std::vector<ovrDrawSurface>& surfaceList) {
    ovrGraphicsCommand& gc = SurfaceDef_.graphicsCommand;
    gc.GpuState.blendMode = BlendMode;
    gc.GpuState.blendSrc = BlendSrc;
    gc.GpuState.blendDst = BlendDst;
    surfaceList.push_back(ovrDrawSurface(ModelMatrix_, &SurfaceDef_));
}

} // namespace OVRFW
