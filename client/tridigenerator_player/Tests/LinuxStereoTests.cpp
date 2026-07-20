#include "Linux/LinuxStereo.h"

#include <cmath>
#include <cstdlib>
#include <iostream>

namespace {

void Check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

bool Near(float actual, float expected, float epsilon = 0.0001f) {
    return std::fabs(actual - expected) <= epsilon;
}

} // namespace

int main() {
    StereoLayout layout{};
    Check(ParseStereoLayout("side-by-side", layout) && layout == StereoLayout::SideBySide,
        "parse side-by-side");
    Check(ParseStereoLayout("over-under", layout) && layout == StereoLayout::OverUnder,
        "parse over-under");
    Check(!ParseStereoLayout("mono", layout), "reject invalid layout");

    auto views = PackedStereoViewports(StereoLayout::SideBySide, 100, 50);
    Check(views[0].width == 50 && views[1].x == 50 && views[1].width == 50,
        "even side-by-side split");
    views = PackedStereoViewports(StereoLayout::OverUnder, 100, 50);
    Check(views[0].y == 25 && views[0].height == 25 && views[1].height == 25,
        "even over-under split");
    views = PackedStereoViewports(StereoLayout::SideBySide, 101, 51);
    Check(views[0].x == 0 && views[0].width == 50, "odd side-by-side left");
    Check(views[1].x == 50 && views[1].width == 51, "odd side-by-side right");
    views = PackedStereoViewports(StereoLayout::OverUnder, 100, 51);
    Check(views[0].y == 25 && views[0].height == 26, "odd over-under top left eye");
    Check(views[1].y == 0 && views[1].height == 25, "odd over-under bottom right eye");

    const XrFovf symmetric{-0.7853981634f, 0.7853981634f, 0.7853981634f, -0.7853981634f};
    const StereoMatrix projection = OpenXrProjection(symmetric, 0.1f, 100.0f);
    Check(Near(projection[0], 1.0f) && Near(projection[5], 1.0f),
        "symmetric projection scale");
    Check(Near(projection[8], 0.0f) && Near(projection[9], 0.0f),
        "symmetric projection center");
    XrFovf asymmetric = symmetric;
    asymmetric.angleRight = std::atan(2.0f);
    Check(OpenXrProjection(asymmetric, 0.1f, 100.0f)[8] > 0.0f,
        "asymmetric projection center");

    XrPosef pose{};
    pose.orientation.w = 1.0f;
    pose.position = {1.0f, 2.0f, 3.0f};
    const StereoMatrix view = OpenXrView(pose, {0.5f, -0.5f, 1.0f});
    Check(Near(view[12], -1.5f) && Near(view[13], -1.5f) && Near(view[14], -4.0f),
        "identity pose and locomotion inverse translation");

    pose = {};
    pose.orientation.y = std::sin(3.1415926536f / 4.0f);
    pose.orientation.w = std::cos(3.1415926536f / 4.0f);
    const StereoMatrix rotated = OpenXrView(pose, {0.0f, 0.0f, 0.0f});
    Check(Near(rotated[0], 0.0f) && Near(rotated[2], 1.0f) && Near(rotated[8], -1.0f),
        "quaternion pose is inverted");
    return 0;
}
