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
    Expect(dataset.maskLabels.size() == 3, "dog-example mask labels parsed");
    Expect(dataset.maskLabels.at(0) == "background", "background mask label");
    Expect(dataset.maskLabels.at(2) == "animal", "animal mask label");
    Expect(dataset.maskLabels.at(3) == "pet", "pet mask label");
    Expect(dataset.HasColorReference(), "schema-v4 dataset has color reference");
    Expect(dataset.colorDecodeProfile == "quest_av1_fast_v1",
        "Quest color decode profile parsed");
    Expect(dataset.colorCodecProfile == "Main" && dataset.colorBitDepth == 8,
        "AV1 Main 8-bit contract parsed");
    Expect(dataset.colorPrimaries == "bt709" &&
        dataset.colorTransfer == "bt709" &&
        dataset.colorMatrix == "bt709" &&
        dataset.colorRange == "limited", "BT.709 limited metadata parsed");
    Expect(!dataset.hasAudio, "manifest remains valid without audio");
    Expect(dataset.depthCodec == "png" &&
        dataset.depthPixelFormat == "gray16be", "legacy depth stream parsed");

    std::string withAudio = ReadFile(VIPE_TEST_MANIFEST);
    const std::string depthStream = "\"pixel_format\": \"gray16be\"\n    }";
    const size_t depthStreamPosition = withAudio.find(depthStream);
    if (depthStreamPosition != std::string::npos) {
        withAudio.insert(
            depthStreamPosition + depthStream.size(),
            R"(,
    "audio": {"index": 3, "codec": "aac", "sample_rate": 44100, "channels": 2})");
    }
    VipeDataset audioDataset;
    error.clear();
    Expect(ParseVipeDataset(withAudio, audioDataset, error), error.c_str());
    Expect(audioDataset.hasAudio && audioDataset.audioStreamIndex == 3,
        "optional audio stream parsed");
    Expect(audioDataset.audioCodec == "aac" && audioDataset.audioSampleRate == 44100 &&
        audioDataset.audioChannels == 2, "audio properties parsed");

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

    std::string legacy = ReadFile(VIPE_TEST_MANIFEST);
    const size_t schemaPosition = legacy.find("\"schema_version\": 4");
    if (schemaPosition != std::string::npos) legacy.replace(
        schemaPosition, std::string("\"schema_version\": 4").size(), "\"schema_version\": 3");
    error.clear();
    Expect(!ParseVipeDataset(legacy, invalid, error),
        "older manifests require re-encoding");
    Expect(error.find("Re-encode") != std::string::npos,
        "older manifest error explains the required action");

    std::string versionFourFfv1 = ReadFile(VIPE_TEST_MANIFEST);
    const size_t pngCodec = versionFourFfv1.find("\"codec\": \"png\"");
    if (pngCodec != std::string::npos) {
        versionFourFfv1.replace(
            pngCodec,
            std::string("\"codec\": \"png\"").size(),
            "\"codec\": \"ffv1\"");
    }
    const size_t bigEndian = versionFourFfv1.find("\"pixel_format\": \"gray16be\"");
    if (bigEndian != std::string::npos) {
        versionFourFfv1.replace(
            bigEndian,
            std::string("\"pixel_format\": \"gray16be\"").size(),
            "\"pixel_format\": \"gray16le\"");
    }
    error.clear();
    Expect(ParseVipeDataset(versionFourFfv1, invalid, error), error.c_str());
    Expect(invalid.depthCodec == "ffv1" &&
        invalid.depthPixelFormat == "gray16le", "schema-v4 FFV1 depth parsed");

    std::string missingReference = ReadFile(VIPE_TEST_MANIFEST);
    const size_t referencePosition = missingReference.find("\"color_reference\"");
    if (referencePosition != std::string::npos) {
        missingReference.replace(
            referencePosition,
            std::string("\"color_reference\"").size(),
            "\"removed_reference\"");
    }
    error.clear();
    Expect(!ParseVipeDataset(missingReference, invalid, error),
        "schema-v4 dataset without color reference rejected");

    return failures == 0 ? 0 : 1;
}
