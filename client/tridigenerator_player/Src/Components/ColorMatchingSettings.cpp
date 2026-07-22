#include "ColorMatchingSettings.h"

#include <cmath>
#include <sstream>

#include <json/json.h>

namespace {
bool InRange(float value, float minimum, float maximum) {
    return std::isfinite(value) && value >= minimum && value <= maximum;
}

bool ReadFloat(const Json::Value& root, const char* name, float& value) {
    if (!root[name].isNumeric()) return false;
    value = root[name].asFloat();
    return std::isfinite(value);
}
} // namespace

bool ValidateColorMatchingSettings(const ColorMatchingSettings& settings, std::string& error) {
    const int tier = static_cast<int>(settings.requestedTier);
    if (tier < static_cast<int>(ColorMatchingTier::Disabled) ||
        tier > static_cast<int>(ColorMatchingTier::Spatial)) {
        error = "tier is outside the supported range";
    } else if (!InRange(settings.matchingStrength, 0.0f, 1.0f)) {
        error = "matching strength must be between 0 and 1";
    } else if (!InRange(settings.temporalSmoothing, 0.0f, 0.95f)) {
        error = "temporal smoothing must be between 0 and 0.95";
    } else if (!InRange(settings.minTint, 0.25f, 1.0f) ||
               !InRange(settings.maxTint, 1.0f, 3.0f) ||
               settings.minTint > settings.maxTint) {
        error = "tint limits are invalid";
    } else if (!InRange(settings.minExposure, 0.1f, 1.0f) ||
               !InRange(settings.maxExposure, 1.0f, 4.0f) ||
               settings.minExposure > settings.maxExposure) {
        error = "exposure limits are invalid";
    } else {
        error.clear();
        return true;
    }
    return false;
}

std::string SerializeColorMatchingSettings(const ColorMatchingSettings& settings) {
    Json::Value root(Json::objectValue);
    root["version"] = 1;
    root["tier"] = static_cast<int>(settings.requestedTier);
    root["matching_strength"] = settings.matchingStrength;
    root["temporal_smoothing"] = settings.temporalSmoothing;
    root["min_tint"] = settings.minTint;
    root["max_tint"] = settings.maxTint;
    root["min_exposure"] = settings.minExposure;
    root["max_exposure"] = settings.maxExposure;
    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";
    return Json::writeString(builder, root);
}

bool ParseColorMatchingSettings(
        const std::string& jsonText, ColorMatchingSettings& settings, std::string& error) {
    Json::CharReaderBuilder builder;
    builder["collectComments"] = false;
    Json::Value root;
    std::istringstream stream(jsonText);
    if (!Json::parseFromStream(builder, stream, &root, &error) || !root.isObject()) {
        error = "invalid settings JSON: " + error;
        return false;
    }
    if (!root["version"].isInt() || root["version"].asInt() != 1 ||
        !root["tier"].isInt()) {
        error = "unsupported or missing settings version/tier";
        return false;
    }
    ColorMatchingSettings parsed;
    parsed.requestedTier = static_cast<ColorMatchingTier>(root["tier"].asInt());
    if (!ReadFloat(root, "matching_strength", parsed.matchingStrength) ||
        !ReadFloat(root, "temporal_smoothing", parsed.temporalSmoothing) ||
        !ReadFloat(root, "min_tint", parsed.minTint) ||
        !ReadFloat(root, "max_tint", parsed.maxTint) ||
        !ReadFloat(root, "min_exposure", parsed.minExposure) ||
        !ReadFloat(root, "max_exposure", parsed.maxExposure) ||
        !ValidateColorMatchingSettings(parsed, error)) {
        if (error.empty()) error = "settings fields must be finite numbers";
        return false;
    }
    settings = parsed;
    return true;
}

