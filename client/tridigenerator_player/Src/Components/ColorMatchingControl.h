#pragma once

enum class ColorMatchingTier : int {
    Disabled = 0,
    Global = 1,
    Spatial = 2,
};

enum class TierAvailability : int {
    Checking = 0,
    Available = 1,
    Unavailable = 2,
};

inline bool ShouldCaptureForColorMatching(ColorMatchingTier tier) {
    return tier != ColorMatchingTier::Disabled;
}

inline bool AllowsSpatialColorMatching(ColorMatchingTier tier) {
    return tier == ColorMatchingTier::Spatial;
}

inline bool IsTierSelectable(ColorMatchingTier tier,
                             TierAvailability globalAvailability,
                             TierAvailability spatialAvailability) {
    if (tier == ColorMatchingTier::Disabled) return true;
    if (tier == ColorMatchingTier::Global) {
        return globalAvailability == TierAvailability::Available;
    }
    return spatialAvailability == TierAvailability::Available;
}

inline ColorMatchingTier DowngradeUnavailableTier(
        ColorMatchingTier requested,
        TierAvailability globalAvailability,
        TierAvailability spatialAvailability) {
    if (requested == ColorMatchingTier::Spatial &&
        spatialAvailability == TierAvailability::Unavailable) {
        if (globalAvailability == TierAvailability::Available) {
            return ColorMatchingTier::Global;
        }
        if (globalAvailability == TierAvailability::Unavailable) {
            return ColorMatchingTier::Disabled;
        }
    }
    if (requested == ColorMatchingTier::Global &&
        globalAvailability == TierAvailability::Unavailable) {
        return ColorMatchingTier::Disabled;
    }
    return requested;
}

inline const char* ColorMatchingTierName(ColorMatchingTier tier) {
    switch (tier) {
        case ColorMatchingTier::Disabled: return "Disabled";
        case ColorMatchingTier::Global: return "Global";
        case ColorMatchingTier::Spatial: return "Spatial";
    }
    return "Disabled";
}

inline const char* TierAvailabilityName(TierAvailability availability) {
    switch (availability) {
        case TierAvailability::Checking: return "Checking...";
        case TierAvailability::Available: return "Available";
        case TierAvailability::Unavailable: return "Unavailable";
    }
    return "Unavailable";
}
