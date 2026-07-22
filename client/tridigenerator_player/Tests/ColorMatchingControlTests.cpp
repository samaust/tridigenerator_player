#include <cassert>

#include "Components/CameraLightEstimationComponent.h"

int main() {
    CameraLightEstimationComponent defaults;
    assert(defaults.requestedTier == ColorMatchingTier::Spatial);
    assert(!ShouldCaptureForColorMatching(ColorMatchingTier::Disabled));
    assert(ShouldCaptureForColorMatching(ColorMatchingTier::Global));
    assert(!AllowsSpatialColorMatching(ColorMatchingTier::Global));
    assert(AllowsSpatialColorMatching(ColorMatchingTier::Spatial));

    assert(IsTierSelectable(
        ColorMatchingTier::Disabled,
        TierAvailability::Unavailable,
        TierAvailability::Unavailable));
    assert(!IsTierSelectable(
        ColorMatchingTier::Global,
        TierAvailability::Checking,
        TierAvailability::Unavailable));
    assert(IsTierSelectable(
        ColorMatchingTier::Global,
        TierAvailability::Available,
        TierAvailability::Unavailable));
    assert(IsTierSelectable(
        ColorMatchingTier::Spatial,
        TierAvailability::Available,
        TierAvailability::Available));

    assert(DowngradeUnavailableTier(
        ColorMatchingTier::Spatial,
        TierAvailability::Available,
        TierAvailability::Unavailable) == ColorMatchingTier::Global);
    assert(DowngradeUnavailableTier(
        ColorMatchingTier::Spatial,
        TierAvailability::Unavailable,
        TierAvailability::Unavailable) == ColorMatchingTier::Disabled);
    assert(DowngradeUnavailableTier(
        ColorMatchingTier::Global,
        TierAvailability::Unavailable,
        TierAvailability::Unavailable) == ColorMatchingTier::Disabled);

    // A tier still being checked is retained rather than downgraded prematurely.
    assert(DowngradeUnavailableTier(
        ColorMatchingTier::Spatial,
        TierAvailability::Checking,
        TierAvailability::Checking) == ColorMatchingTier::Spatial);

    // Transient loss affects the achieved tier, not capability or selection.
    assert(DowngradeUnavailableTier(
        ColorMatchingTier::Spatial,
        TierAvailability::Available,
        TierAvailability::Available) == ColorMatchingTier::Spatial);
    return 0;
}
