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

Filename    :   UnlitGeometryRenderer.h
Content     :   Simple rendering for geometry-based types
Created     :   Mar 2021
Authors     :   Federico Schliemann
Language    :   C++

*******************************************************************************/

#pragma once
#include <vector>
#include <cstdint>

#include "OVR_Math.h"
#include "SurfaceRender.h"
#include "GeometryBuilder.h"

namespace OVRFW {

    class UnlitGeometryRenderer {
    public:
        UnlitGeometryRenderer() = default;
        virtual ~UnlitGeometryRenderer() = default;

        virtual void Init(const GlGeometry::Descriptor& d);
        virtual void Shutdown();
        virtual void Update();
        virtual void Render(std::vector<ovrDrawSurface>& surfaceList);
        virtual void UpdateGeometry(const GlGeometry::Descriptor& d);

        void CreateTextures(
                uint32_t textureYWidth,
                uint32_t textureYHeight,
                uint32_t textureUWidth,
                uint32_t textureUHeight,
                uint32_t textureVWidth,
                uint32_t textureVHeight);

        void UpdateTextures(
                const uint8_t* textureYData,
                uint32_t textureYWidth,
                uint32_t textureYHeight,
                const uint8_t* textureUData,
                uint32_t textureUWidth,
                uint32_t textureUHeight,
                const uint8_t* textureVData,
                uint32_t textureVWidth,
                uint32_t textureVHeight);

        void SetPose(const OVR::Posef& pose) {
            ModelPose_ = pose;
        }

        OVR::Posef GetPose() {
            return ModelPose_;
        }

        void SetScale(OVR::Vector3f v) {
            ModelScale_ = v;
        }

        OVR::Vector3f GetScale() {
            return ModelScale_;
        }

        bool IsValid() const {
            return (textures_[0][0] != 0 && textures_[0][1] != 0 && textures_[0][2] != 0);
        }

        OVR::Vector4f ChannelControl = {1, 1, 1, 1};
        OVR::Vector4f DiffuseColor = {0.4, 1.0, 0.2, 1.0};
        OVR::Vector3f SpecularLightDirection = OVR::Vector3f{1, 1, 1}.Normalized();
        OVR::Vector3f SpecularLightColor = {1, 1, 1};
        OVR::Vector3f AmbientLightColor = {.1, .1, .1};
        uint32_t BlendSrc = ovrGpuState::kGL_SRC_ALPHA;
        uint32_t BlendDst = ovrGpuState::kGL_ONE_MINUS_SRC_ALPHA;
        uint32_t BlendMode = ovrGpuState::kGL_FUNC_ADD;

    private:
        //ovrSurfaceDef SurfaceDef_;

        // Double-buffered surface definitions
        ovrSurfaceDef surfaceDefs_[2];
        int currentSurfaceSet_ = 0;

        GlProgram Program_;
        OVR::Matrix4f ModelMatrix_ = OVR::Matrix4f::Identity();
        OVR::Vector3f ModelScale_ = {1, 1, 1};
        OVR::Posef ModelPose_ = OVR::Posef::Identity();
        //OVRFW::GlTexture TexY_;
        //OVRFW::GlTexture TexU_;
        //OVRFW::GlTexture TexV_;

        // Double-buffered textures
        OVRFW::GlTexture textures_[2][3]; // [buffer_index][Y=0, U=1, V=2]

        GlTexture CreateGlTexture(uint32_t pixelWidth, uint32_t pixelHeight);
        void UpdateGlTexture(GlTexture texture, const uint8_t* textureData);
    };

} // namespace OVRFW

//// --------------------------------
