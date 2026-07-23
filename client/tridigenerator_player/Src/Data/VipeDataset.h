#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

struct VipeFrameMetadata {
    std::array<float, 4> intrinsics{}; // fx, fy, cx, cy
    std::array<float, 16> cameraToWorld{}; // row-major OpenCV convention
};

struct VipeColorReference {
    std::array<float, 3> chromaticity{};
    float logAverageLuminance = 0.0f;
    uint64_t sampleCount = 0;
};

struct VipeDatasetColorReferences {
    std::string colorSpace;
    std::string aggregation;
    VipeColorReference global;
    std::unordered_map<uint8_t, VipeColorReference> masks;
};

struct VipeDataset {
    int schemaVersion = 0;
    std::string sequence;
    std::string videoFile;
    int width = 0;
    int height = 0;
    int frameCount = 0;
    int frameRateNumerator = 0;
    int frameRateDenominator = 1;
    float depthUnitsPerMetre = 0.0f;
    uint16_t invalidDepthValue = 0;
    // World-space OpenGL orientation correction in degrees: yaw (+Y), pitch (+X), roll (+Z).
    std::array<float, 3> orientationOffsetDegrees{};
    std::vector<VipeFrameMetadata> frames;
    std::unordered_map<uint8_t, std::string> maskLabels;
    VipeDatasetColorReferences colorReferences;
    bool hasAudio = false;
    int audioStreamIndex = -1;
    std::string audioCodec;
    int audioSampleRate = 0;
    int audioChannels = 0;

    bool HasColorReference() const { return schemaVersion >= 2; }
};

struct VipeCatalogEntry {
    std::string id;
    std::string displayName;
    std::string manifest;
};

struct VipeCatalog {
    int schemaVersion = 0;
    std::vector<VipeCatalogEntry> datasets;
};

bool ParseVipeDataset(
    const std::string& jsonText,
    VipeDataset& dataset,
    std::string& error);

bool ParseVipeCatalog(
    const std::string& jsonText,
    VipeCatalog& catalog,
    std::string& error);

// Returns C * cameraToWorld * inverse(firstCameraToWorld) * C^-1 where
// C converts OpenCV (+X right, +Y down, +Z forward) to OpenGL.
std::array<float, 16> RelativeOpenGlCameraPose(
    const std::array<float, 16>& cameraToWorld,
    const std::array<float, 16>& firstCameraToWorld);

// Applies the manifest's world-space yaw, pitch, and roll correction to the
// frame-0-anchored OpenGL pose. Rotation order is yaw * pitch * roll.
std::array<float, 16> OrientedRelativeOpenGlCameraPose(
    const std::array<float, 16>& cameraToWorld,
    const std::array<float, 16>& firstCameraToWorld,
    const std::array<float, 3>& orientationOffsetDegrees);
