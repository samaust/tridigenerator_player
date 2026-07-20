#include "LinuxStereo.h"

#include <cmath>

bool ParseStereoLayout(const std::string& value, StereoLayout& layout) {
    if (value == "side-by-side") {
        layout = StereoLayout::SideBySide;
        return true;
    }
    if (value == "over-under") {
        layout = StereoLayout::OverUnder;
        return true;
    }
    return false;
}

std::array<StereoViewport, 2> PackedStereoViewports(
    StereoLayout layout, int width, int height) {
    if (layout == StereoLayout::SideBySide) {
        const int leftWidth = width / 2;
        return {{{0, 0, leftWidth, height}, {leftWidth, 0, width - leftWidth, height}}};
    }
    const int bottomHeight = height / 2;
    return {{{0, bottomHeight, width, height - bottomHeight}, {0, 0, width, bottomHeight}}};
}

StereoMatrix OpenXrProjection(const XrFovf& fov, float nearPlane, float farPlane) {
    const float tanLeft = std::tan(fov.angleLeft);
    const float tanRight = std::tan(fov.angleRight);
    const float tanDown = std::tan(fov.angleDown);
    const float tanUp = std::tan(fov.angleUp);
    const float width = tanRight - tanLeft;
    const float height = tanUp - tanDown;
    StereoMatrix result{};
    result[0] = 2.0f / width;
    result[5] = 2.0f / height;
    result[8] = (tanRight + tanLeft) / width;
    result[9] = (tanUp + tanDown) / height;
    result[10] = -(farPlane + nearPlane) / (farPlane - nearPlane);
    result[11] = -1.0f;
    result[14] = -(2.0f * farPlane * nearPlane) / (farPlane - nearPlane);
    return result;
}

StereoMatrix OpenXrView(const XrPosef& pose, const std::array<float, 3>& offset) {
    const float x = pose.orientation.x;
    const float y = pose.orientation.y;
    const float z = pose.orientation.z;
    const float w = pose.orientation.w;

    // Inverse of the pose's rigid transform: transpose the quaternion rotation,
    // then translate by the negative tracked position and locomotion offset.
    StereoMatrix result{
        1.0f - 2.0f * (y * y + z * z), 2.0f * (x * y - z * w),
        2.0f * (x * z + y * w), 0.0f,
        2.0f * (x * y + z * w), 1.0f - 2.0f * (x * x + z * z),
        2.0f * (y * z - x * w), 0.0f,
        2.0f * (x * z - y * w), 2.0f * (y * z + x * w),
        1.0f - 2.0f * (x * x + y * y), 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f};
    const float px = pose.position.x + offset[0];
    const float py = pose.position.y + offset[1];
    const float pz = pose.position.z + offset[2];
    result[12] = -(result[0] * px + result[4] * py + result[8] * pz);
    result[13] = -(result[1] * px + result[5] * py + result[9] * pz);
    result[14] = -(result[2] * px + result[6] * py + result[10] * pz);
    return result;
}
