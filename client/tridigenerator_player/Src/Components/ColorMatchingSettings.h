#pragma once

#include <string>

#include "ColorMatchingControl.h"

struct ColorMatchingSettings {
    ColorMatchingTier requestedTier = ColorMatchingTier::Spatial;
    float matchingStrength = 1.0f;
    float temporalSmoothing = 0.85f;
    float minTint = 0.7f;
    float maxTint = 1.4f;
    float minExposure = 0.35f;
    float maxExposure = 2.0f;
};

inline bool operator==(const ColorMatchingSettings& a, const ColorMatchingSettings& b) {
    return a.requestedTier == b.requestedTier &&
        a.matchingStrength == b.matchingStrength &&
        a.temporalSmoothing == b.temporalSmoothing &&
        a.minTint == b.minTint && a.maxTint == b.maxTint &&
        a.minExposure == b.minExposure && a.maxExposure == b.maxExposure;
}

inline bool operator!=(const ColorMatchingSettings& a, const ColorMatchingSettings& b) {
    return !(a == b);
}

bool ValidateColorMatchingSettings(const ColorMatchingSettings& settings, std::string& error);
std::string SerializeColorMatchingSettings(const ColorMatchingSettings& settings);
bool ParseColorMatchingSettings(
    const std::string& jsonText, ColorMatchingSettings& settings, std::string& error);

