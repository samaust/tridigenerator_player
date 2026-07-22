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
        dataset.orientationOffsetDegrees[1] == -45.0f &&
        dataset.orientationOffsetDegrees[2] == 0.0f, "orientation offsets parsed");
    Expect(dataset.maskLabels.size() == 3, "dog-example mask labels parsed");
    Expect(dataset.maskLabels.at(0) == "background", "background mask label");
    Expect(dataset.maskLabels.at(2) == "animal", "animal mask label");
    Expect(dataset.maskLabels.at(3) == "pet", "pet mask label");
    Expect(!dataset.HasColorReference(), "schema-v1 dataset has no color reference");

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

    std::string versionTwo = ReadFile(VIPE_TEST_MANIFEST);
    const size_t schemaPosition = versionTwo.find("\"schema_version\": 1");
    if (schemaPosition != std::string::npos) versionTwo.replace(
        schemaPosition, std::string("\"schema_version\": 1").size(), "\"schema_version\": 2");
    const size_t finalBrace = versionTwo.find_last_of('}');
    if (finalBrace != std::string::npos) versionTwo.insert(finalBrace,
        R"(,"color_reference":{"color_space":"linear_srgb","aggregation":"sequence","global":{"chromaticity":[1.0,1.0,1.0],"log_average_luminance":0.25,"sample_count":4096},"masks":{"2":{"chromaticity":[1.2,0.95,0.85],"log_average_luminance":0.2,"sample_count":2048}}}})");
    error.clear();
    Expect(ParseVipeDataset(versionTwo, invalid, error), error.c_str());
    Expect(invalid.HasColorReference(), "schema-v2 dataset has color reference");
    Expect(invalid.colorReferences.masks.size() == 1, "per-mask color reference parsed");

    std::string missingReference = versionTwo;
    const size_t referencePosition = missingReference.find(",\"color_reference\"");
    if (referencePosition != std::string::npos) missingReference.erase(
        referencePosition, missingReference.find_last_of('}') - referencePosition);
    error.clear();
    Expect(!ParseVipeDataset(missingReference, invalid, error),
        "schema-v2 dataset without color reference rejected");

    return failures == 0 ? 0 : 1;
}
