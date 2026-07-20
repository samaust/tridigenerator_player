#pragma once

#include <array>
#include <string>

#include <openxr/openxr.h>

enum class StereoLayout {
    SideBySide,
    OverUnder,
};

struct StereoViewport {
    int x;
    int y;
    int width;
    int height;
};

bool ParseStereoLayout(const std::string& value, StereoLayout& layout);
std::array<StereoViewport, 2> PackedStereoViewports(
    StereoLayout layout, int width, int height);

using StereoMatrix = std::array<float, 16>;

StereoMatrix OpenXrProjection(const XrFovf& fov, float nearPlane, float farPlane);
StereoMatrix OpenXrView(const XrPosef& pose, const std::array<float, 3>& locomotionOffset);
