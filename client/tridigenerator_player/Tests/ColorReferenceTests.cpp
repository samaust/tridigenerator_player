#include "Data/ColorReference.h"

#include <cassert>
#include <cmath>

int main() {
    VipeDataset dataset;
    dataset.schemaVersion = 2;
    dataset.colorReferences.global = {{1.0f, 1.0f, 1.0f}, 0.2f, 4096};
    dataset.colorReferences.masks.emplace(
        7, VipeColorReference{{1.2f, 0.9f, 0.8f}, 0.4f, 2048});
    const auto lookup = BuildColorReferenceLookup(dataset);
    assert(lookup[0][3] == 0.2f);
    assert(lookup[6] == lookup[0]);
    assert(lookup[7][0] == 1.2f && lookup[7][3] == 0.4f);

    const auto neutral = CalculateMatchingGain(
        {1.0f, 1.0f, 1.0f, 0.2f}, lookup[0], 0.7f, 1.4f, 0.35f, 2.0f);
    assert(std::abs(neutral[0] - 1.0f) < 1.0e-6f);
    const auto clamped = CalculateMatchingGain(
        {4.0f, 0.1f, 1.0f, 2.0f}, lookup[0], 0.7f, 1.4f, 0.35f, 2.0f);
    assert(std::abs(clamped[0] - 2.8f) < 1.0e-6f);
    assert(std::abs(clamped[1] - 1.4f) < 1.0e-6f);

    VipeDataset legacy;
    const auto unavailable = BuildColorReferenceLookup(legacy);
    assert(unavailable[255][0] == 1.0f && unavailable[255][3] == 0.18f);
    return 0;
}
