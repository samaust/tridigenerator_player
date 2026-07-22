#include "Components/ColorMatchingSettings.h"

#include <cassert>
#include <limits>
#include <string>
#include <unordered_map>

int main() {
    const ColorMatchingSettings defaults;
    assert(defaults.requestedTier == ColorMatchingTier::Spatial);
    assert(defaults.matchingStrength == 1.0f);
    assert(defaults.temporalSmoothing == 0.85f);
    assert(defaults.minTint == 0.7f && defaults.maxTint == 1.4f);
    assert(defaults.minExposure == 0.35f && defaults.maxExposure == 2.0f);

    std::string error;
    assert(ValidateColorMatchingSettings(defaults, error));
    ColorMatchingSettings parsed;
    assert(ParseColorMatchingSettings(SerializeColorMatchingSettings(defaults), parsed, error));
    assert(parsed == defaults);

    parsed.matchingStrength = std::numeric_limits<float>::quiet_NaN();
    assert(!ValidateColorMatchingSettings(parsed, error));
    parsed = defaults;
    parsed.minTint = 1.0f;
    parsed.maxTint = 0.9f;
    assert(!ValidateColorMatchingSettings(parsed, error));
    parsed = defaults;
    parsed.minExposure = 1.0f;
    parsed.maxExposure = 0.9f;
    assert(!ValidateColorMatchingSettings(parsed, error));

    assert(!ParseColorMatchingSettings("{}", parsed, error));
    assert(!ParseColorMatchingSettings(
        R"({"version":2,"tier":2,"matching_strength":1,"temporal_smoothing":0.85,"min_tint":0.7,"max_tint":1.4,"min_exposure":0.35,"max_exposure":2})",
        parsed, error));
    assert(!ParseColorMatchingSettings(
        R"({"version":1,"tier":99,"matching_strength":1,"temporal_smoothing":0.85,"min_tint":0.7,"max_tint":1.4,"min_exposure":0.35,"max_exposure":2})",
        parsed, error));

    ColorMatchingSettings custom = defaults;
    custom.requestedTier = ColorMatchingTier::Global;
    custom.matchingStrength = 0.5f;
    assert(ParseColorMatchingSettings(SerializeColorMatchingSettings(custom), parsed, error));
    assert(parsed == custom);
    custom = ColorMatchingSettings{};
    assert(custom == defaults);

    // SharedPreferences is a per-dataset string map; exercise independent values and fallback.
    std::unordered_map<std::string, std::string> storage;
    ColorMatchingSettings dog = defaults;
    dog.requestedTier = ColorMatchingTier::Global;
    dog.matchingStrength = 0.6f;
    ColorMatchingSettings cat = defaults;
    cat.requestedTier = ColorMatchingTier::Disabled;
    storage["dog"] = SerializeColorMatchingSettings(dog);
    storage["cat"] = SerializeColorMatchingSettings(cat);
    assert(ParseColorMatchingSettings(storage["dog"], parsed, error) && parsed == dog);
    assert(ParseColorMatchingSettings(storage["cat"], parsed, error) && parsed == cat);
    storage["broken"] = "not json";
    parsed = defaults;
    assert(!ParseColorMatchingSettings(storage["broken"], parsed, error));
    assert(parsed == defaults);
    return 0;
}
