#include "VipeDataset.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <sstream>
#include <stdexcept>

#include <json/json.h>

namespace {

bool ReadFiniteFloat(const Json::Value& value, float& output) {
    if (!value.isNumeric()) {
        return false;
    }
    output = value.asFloat();
    return std::isfinite(output);
}

bool ReadMatrix(const Json::Value& value, std::array<float, 16>& output) {
    if (!value.isArray() || value.size() != output.size()) {
        return false;
    }
    for (Json::ArrayIndex i = 0; i < value.size(); ++i) {
        if (!ReadFiniteFloat(value[i], output[i])) {
            return false;
        }
    }
    return true;
}

bool ReadColorReference(const Json::Value& value, VipeColorReference& output) {
    if (!value.isObject() || !value["chromaticity"].isArray() ||
        value["chromaticity"].size() != 3 || !value["sample_count"].isUInt64() ||
        value["sample_count"].asUInt64() == 0 ||
        !ReadFiniteFloat(value["log_average_luminance"], output.logAverageLuminance) ||
        output.logAverageLuminance <= 0.0f) {
        return false;
    }
    for (Json::ArrayIndex i = 0; i < 3; ++i) {
        if (!ReadFiniteFloat(value["chromaticity"][i], output.chromaticity[i]) ||
            output.chromaticity[i] <= 0.0f) {
            return false;
        }
    }
    output.sampleCount = value["sample_count"].asUInt64();
    const float luminance = 0.2126f * output.chromaticity[0] +
        0.7152f * output.chromaticity[1] + 0.0722f * output.chromaticity[2];
    return std::abs(luminance - 1.0f) <= 0.02f;
}

std::array<float, 16> Multiply(
    const std::array<float, 16>& a,
    const std::array<float, 16>& b) {
    std::array<float, 16> result{};
    for (int row = 0; row < 4; ++row) {
        for (int column = 0; column < 4; ++column) {
            for (int k = 0; k < 4; ++k) {
                result[row * 4 + column] += a[row * 4 + k] * b[k * 4 + column];
            }
        }
    }
    return result;
}

std::array<float, 16> InvertRigid(const std::array<float, 16>& matrix) {
    std::array<float, 16> result{
        matrix[0], matrix[4], matrix[8], 0.0f,
        matrix[1], matrix[5], matrix[9], 0.0f,
        matrix[2], matrix[6], matrix[10], 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f};
    result[3] = -(result[0] * matrix[3] + result[1] * matrix[7] + result[2] * matrix[11]);
    result[7] = -(result[4] * matrix[3] + result[5] * matrix[7] + result[6] * matrix[11]);
    result[11] = -(result[8] * matrix[3] + result[9] * matrix[7] + result[10] * matrix[11]);
    return result;
}

bool IsRigidTransform(const std::array<float, 16>& matrix) {
    constexpr float epsilon = 1.0e-3f;
    if (std::abs(matrix[12]) > epsilon || std::abs(matrix[13]) > epsilon ||
        std::abs(matrix[14]) > epsilon || std::abs(matrix[15] - 1.0f) > epsilon) {
        return false;
    }
    for (int row = 0; row < 3; ++row) {
        float lengthSquared = 0.0f;
        for (int column = 0; column < 3; ++column) {
            lengthSquared += matrix[row * 4 + column] * matrix[row * 4 + column];
        }
        if (std::abs(lengthSquared - 1.0f) > 5.0e-3f) {
            return false;
        }
    }
    return true;
}

bool ParseJson(const std::string& text, Json::Value& root, std::string& error) {
    Json::CharReaderBuilder builder;
    builder["collectComments"] = false;
    std::istringstream stream(text);
    if (!Json::parseFromStream(builder, stream, &root, &error)) {
        error = "Invalid JSON: " + error;
        return false;
    }
    if (!root.isObject()) {
        error = "JSON root must be an object";
        return false;
    }
    return true;
}

} // namespace

bool ParseVipeDataset(const std::string& jsonText, VipeDataset& dataset, std::string& error) {
    Json::Value root;
    if (!ParseJson(jsonText, root, error)) {
        return false;
    }
    VipeDataset parsed;
    if (!root["schema_version"].isInt() ||
        (root["schema_version"].asInt() != 1 && root["schema_version"].asInt() != 2)) {
        error = "Only ViPE manifest schema_version 1 and 2 are supported";
        return false;
    }
    parsed.schemaVersion = root["schema_version"].asInt();
    if (!root["sequence"].isString() || root["sequence"].asString().empty() ||
        !root["file"].isString() || root["file"].asString().empty()) {
        error = "Manifest requires non-empty sequence and file strings";
        return false;
    }
    parsed.sequence = root["sequence"].asString();
    parsed.videoFile = root["file"].asString();
    if (!root["width"].isInt() || !root["height"].isInt() ||
        !root["frame_count"].isInt()) {
        error = "Manifest requires integer width, height, and frame_count";
        return false;
    }
    parsed.width = root["width"].asInt();
    parsed.height = root["height"].asInt();
    parsed.frameCount = root["frame_count"].asInt();
    if (parsed.width <= 1 || parsed.height <= 1 || parsed.frameCount <= 0) {
        error = "Manifest dimensions and frame_count must be positive";
        return false;
    }

    const Json::Value& frameRate = root["frame_rate"];
    if (!frameRate.isObject() || !frameRate["numerator"].isInt() ||
        !frameRate["denominator"].isInt()) {
        error = "Manifest requires frame_rate numerator and denominator";
        return false;
    }
    parsed.frameRateNumerator = frameRate["numerator"].asInt();
    parsed.frameRateDenominator = frameRate["denominator"].asInt();
    if (parsed.frameRateNumerator <= 0 || parsed.frameRateDenominator <= 0) {
        error = "Frame rate numerator and denominator must be positive";
        return false;
    }

    const Json::Value& streams = root["streams"];
    const auto streamMatches = [&](const char* name, int index, const char* codec, const char* format) {
        const Json::Value& stream = streams[name];
        return stream.isObject() && stream["index"].asInt() == index &&
            stream["codec"].asString() == codec && stream["pixel_format"].asString() == format;
    };
    if (!streams.isObject() || !streamMatches("color", 0, "av1", "yuv420p") ||
        !streamMatches("mask", 1, "ffv1", "gray") ||
        !streamMatches("depth", 2, "png", "gray16be")) {
        error = "Manifest stream contract must be AV1 color, FFV1 gray mask, and PNG gray16be depth";
        return false;
    }

    const Json::Value& depth = root["depth"];
    if (!depth.isObject() || depth["encoding"].asString() != "uint16_linear" ||
        depth["units"].asString() != "metres" ||
        !ReadFiniteFloat(depth["units_per_metre"], parsed.depthUnitsPerMetre) ||
        parsed.depthUnitsPerMetre <= 0.0f || !depth["invalid_value"].isUInt() ||
        depth["invalid_value"].asUInt() > std::numeric_limits<uint16_t>::max()) {
        error = "Manifest has invalid uint16 metric depth metadata";
        return false;
    }
    parsed.invalidDepthValue = static_cast<uint16_t>(depth["invalid_value"].asUInt());

    const Json::Value& orientation = root["orientation_offset_degrees"];
    if (!orientation.isNull()) {
        if (!orientation.isObject() ||
            !ReadFiniteFloat(orientation["yaw"], parsed.orientationOffsetDegrees[0]) ||
            !ReadFiniteFloat(orientation["pitch"], parsed.orientationOffsetDegrees[1]) ||
            !ReadFiniteFloat(orientation["roll"], parsed.orientationOffsetDegrees[2])) {
            error = "orientation_offset_degrees requires finite yaw, pitch, and roll values";
            return false;
        }
    }

    const Json::Value& pose = root["pose"];
    const Json::Value& intrinsics = root["intrinsics"];
    if (!pose.isObject() || pose["type"].asString() != "camera_to_world" ||
        pose["matrix_layout"].asString() != "row_major" ||
        pose["coordinate_convention"].asString() != "opencv_x_right_y_down_z_forward" ||
        !pose["frame_indices"].isArray() || !pose["matrices"].isArray() ||
        !intrinsics.isObject() || !intrinsics["frame_indices"].isArray() ||
        !intrinsics["values"].isArray() || !intrinsics["camera_models"].isArray()) {
        error = "Manifest has invalid pose or intrinsics metadata";
        return false;
    }
    if (pose["matrices"].size() != static_cast<Json::ArrayIndex>(parsed.frameCount) ||
        pose["frame_indices"].size() != static_cast<Json::ArrayIndex>(parsed.frameCount) ||
        intrinsics["values"].size() != static_cast<Json::ArrayIndex>(parsed.frameCount) ||
        intrinsics["frame_indices"].size() != static_cast<Json::ArrayIndex>(parsed.frameCount) ||
        intrinsics["camera_models"].size() != static_cast<Json::ArrayIndex>(parsed.frameCount)) {
        error = "Pose and intrinsics arrays must match frame_count";
        return false;
    }

    parsed.frames.resize(parsed.frameCount);
    for (int frame = 0; frame < parsed.frameCount; ++frame) {
        if (!pose["frame_indices"][frame].isInt() || pose["frame_indices"][frame].asInt() != frame ||
            !intrinsics["frame_indices"][frame].isInt() ||
            intrinsics["frame_indices"][frame].asInt() != frame) {
            error = "Pose and intrinsics frame indices must be contiguous and zero-based";
            return false;
        }
        if (intrinsics["camera_models"][frame].asString() != "PINHOLE") {
            error = "Only PINHOLE ViPE camera models are supported";
            return false;
        }
        if (!ReadMatrix(pose["matrices"][frame], parsed.frames[frame].cameraToWorld) ||
            !IsRigidTransform(parsed.frames[frame].cameraToWorld)) {
            error = "Invalid rigid camera-to-world matrix at frame " + std::to_string(frame);
            return false;
        }
        const Json::Value& values = intrinsics["values"][frame];
        if (!values.isArray() || values.size() != 4) {
            error = "Invalid pinhole intrinsics at frame " + std::to_string(frame);
            return false;
        }
        for (Json::ArrayIndex i = 0; i < 4; ++i) {
            if (!ReadFiniteFloat(values[i], parsed.frames[frame].intrinsics[i])) {
                error = "Non-finite intrinsics at frame " + std::to_string(frame);
                return false;
            }
        }
        if (parsed.frames[frame].intrinsics[0] <= 0.0f || parsed.frames[frame].intrinsics[1] <= 0.0f) {
            error = "Focal lengths must be positive at frame " + std::to_string(frame);
            return false;
        }
    }

    const Json::Value& labels = root["mask_labels"];
    if (labels.isObject()) {
        for (const std::string& key : labels.getMemberNames()) {
            try {
                const int id = std::stoi(key);
                if (id < 0 || id > 255 || !labels[key].isString()) {
                    throw std::invalid_argument("invalid mask label");
                }
                parsed.maskLabels.emplace(static_cast<uint8_t>(id), labels[key].asString());
            } catch (const std::exception&) {
                error = "Mask labels must map uint8 string keys to string values";
                return false;
            }
        }
    }
    if (parsed.schemaVersion == 2) {
        const Json::Value& references = root["color_reference"];
        if (!references.isObject() || references["color_space"].asString() != "linear_srgb" ||
            references["aggregation"].asString() != "sequence" ||
            !ReadColorReference(references["global"], parsed.colorReferences.global) ||
            !references["masks"].isObject()) {
            error = "Schema version 2 requires valid sequence-aggregated linear_srgb color_reference metadata";
            return false;
        }
        parsed.colorReferences.colorSpace = "linear_srgb";
        parsed.colorReferences.aggregation = "sequence";
        for (const std::string& key : references["masks"].getMemberNames()) {
            try {
                size_t consumed = 0;
                const int id = std::stoi(key, &consumed);
                VipeColorReference reference;
                if (consumed != key.size() || id < 0 || id > 255 ||
                    !ReadColorReference(references["masks"][key], reference)) {
                    throw std::invalid_argument("invalid mask color reference");
                }
                parsed.colorReferences.masks.emplace(static_cast<uint8_t>(id), reference);
            } catch (const std::exception&) {
                error = "Color reference masks must map uint8 string keys to valid references";
                return false;
            }
        }
    }
    dataset = std::move(parsed);
    return true;
}

bool ParseVipeCatalog(const std::string& jsonText, VipeCatalog& catalog, std::string& error) {
    Json::Value root;
    if (!ParseJson(jsonText, root, error)) {
        return false;
    }
    if (root["schema_version"].asInt() != 1 || !root["datasets"].isArray()) {
        error = "Catalog requires schema_version 1 and a datasets array";
        return false;
    }
    VipeCatalog parsed;
    parsed.schemaVersion = 1;
    for (const Json::Value& item : root["datasets"]) {
        VipeCatalogEntry entry;
        if (!item.isObject() || !item["id"].isString() || !item["display_name"].isString() ||
            !item["manifest"].isString()) {
            error = "Every catalog dataset requires id, display_name, and manifest strings";
            return false;
        }
        entry.id = item["id"].asString();
        entry.displayName = item["display_name"].asString();
        entry.manifest = item["manifest"].asString();
        if (entry.id.empty() || entry.displayName.empty() || entry.manifest.empty()) {
            error = "Catalog dataset fields must not be empty";
            return false;
        }
        const auto duplicate = std::find_if(
            parsed.datasets.begin(), parsed.datasets.end(),
            [&](const VipeCatalogEntry& existing) { return existing.id == entry.id; });
        if (duplicate != parsed.datasets.end()) {
            error = "Catalog dataset IDs must be unique";
            return false;
        }
        parsed.datasets.push_back(std::move(entry));
    }
    if (parsed.datasets.empty()) {
        error = "Catalog must contain at least one dataset";
        return false;
    }
    catalog = std::move(parsed);
    return true;
}

std::array<float, 16> RelativeOpenGlCameraPose(
    const std::array<float, 16>& cameraToWorld,
    const std::array<float, 16>& firstCameraToWorld) {
    const std::array<float, 16> conversion{
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, -1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, -1.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f};
    return Multiply(Multiply(Multiply(conversion, cameraToWorld), InvertRigid(firstCameraToWorld)), conversion);
}

std::array<float, 16> OrientedRelativeOpenGlCameraPose(
    const std::array<float, 16>& cameraToWorld,
    const std::array<float, 16>& firstCameraToWorld,
    const std::array<float, 3>& orientationOffsetDegrees) {
    constexpr float degreesToRadians = 3.14159265358979323846f / 180.0f;
    const float yaw = orientationOffsetDegrees[0] * degreesToRadians;
    const float pitch = orientationOffsetDegrees[1] * degreesToRadians;
    const float roll = orientationOffsetDegrees[2] * degreesToRadians;
    const float cy = std::cos(yaw), sy = std::sin(yaw);
    const float cp = std::cos(pitch), sp = std::sin(pitch);
    const float cr = std::cos(roll), sr = std::sin(roll);
    const std::array<float, 16> yawRotation{
        cy, 0.0f, sy, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
        -sy, 0.0f, cy, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f};
    const std::array<float, 16> pitchRotation{
        1.0f, 0.0f, 0.0f, 0.0f, 0.0f, cp, -sp, 0.0f,
        0.0f, sp, cp, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f};
    const std::array<float, 16> rollRotation{
        cr, -sr, 0.0f, 0.0f, sr, cr, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f};
    const auto offset = Multiply(Multiply(yawRotation, pitchRotation), rollRotation);
    return Multiply(offset, RelativeOpenGlCameraPose(cameraToWorld, firstCameraToWorld));
}
