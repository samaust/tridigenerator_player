#include "Data/VipeDataset.h"

#include <cmath>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

namespace {

int failures = 0;

void Expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

std::string ReadFile(const char* path) {
    std::ifstream input(path);
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

} // namespace

int main() {
    VipeDataset dataset;
    std::string error;
    Expect(ParseVipeDataset(ReadFile(VIPE_TEST_MANIFEST), dataset, error), error.c_str());
    Expect(dataset.frameCount == 122, "dog-example frame count");
    Expect(dataset.width == 1280 && dataset.height == 720, "dog-example dimensions");
    Expect(dataset.frameRateNumerator == 2997 && dataset.frameRateDenominator == 100,
        "rational frame rate");
    Expect(dataset.frames.size() == 122, "per-frame metadata count");
    Expect(dataset.depthUnitsPerMetre > 8000.0f, "depth scale parsed");
    Expect(dataset.orientationOffsetDegrees[0] == 0.0f &&
        dataset.orientationOffsetDegrees[1] == 0.0f &&
        dataset.orientationOffsetDegrees[2] == 0.0f, "orientation offsets parsed");

    if (!dataset.frames.empty()) {
        const auto identity = RelativeOpenGlCameraPose(
            dataset.frames.front().cameraToWorld, dataset.frames.front().cameraToWorld);
        for (int i = 0; i < 16; ++i) {
            const float expected = (i % 5 == 0) ? 1.0f : 0.0f;
            Expect(std::abs(identity[i] - expected) < 1.0e-4f, "first pose anchors to identity");
        }

        const auto yaw90 = OrientedRelativeOpenGlCameraPose(
            dataset.frames.front().cameraToWorld,
            dataset.frames.front().cameraToWorld,
            {90.0f, 0.0f, 0.0f});
        Expect(std::abs(yaw90[0]) < 1.0e-4f && std::abs(yaw90[2] - 1.0f) < 1.0e-4f &&
            std::abs(yaw90[8] + 1.0f) < 1.0e-4f && std::abs(yaw90[10]) < 1.0e-4f,
            "yaw orientation offset rotates around OpenGL +Y");
    }

    VipeCatalog catalog;
    error.clear();
    Expect(ParseVipeCatalog(
        R"({"schema_version":1,"datasets":[{"id":"dog-example","display_name":"Dog Example","manifest":"/vipe_encoded/dog-example.json"}]})",
        catalog, error), error.c_str());
    Expect(catalog.datasets.size() == 1, "catalog entry parsed");

    VipeDataset invalid;
    error.clear();
    Expect(!ParseVipeDataset("{}", invalid, error), "missing manifest fields rejected");

    std::string mismatched = ReadFile(VIPE_TEST_MANIFEST);
    const std::string needle = "\"frame_count\": 122";
    const size_t position = mismatched.find(needle);
    if (position != std::string::npos) mismatched.replace(position, needle.size(), "\"frame_count\": 121");
    error.clear();
    Expect(!ParseVipeDataset(mismatched, invalid, error), "mismatched frame metadata rejected");

    return failures == 0 ? 0 : 1;
}
