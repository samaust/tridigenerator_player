#pragma once

#include <algorithm>
#include <array>

#include "VipeDataset.h"

using ColorReferenceLookup = std::array<std::array<float, 4>, 256>;

inline ColorReferenceLookup BuildColorReferenceLookup(const VipeDataset& dataset) {
    ColorReferenceLookup lookup{};
    if (!dataset.HasColorReference()) {
        lookup.fill({1.0f, 1.0f, 1.0f, 0.18f});
        return lookup;
    }
    const auto pack = [](const VipeColorReference& reference) {
        return std::array<float, 4>{reference.chromaticity[0], reference.chromaticity[1],
            reference.chromaticity[2], reference.logAverageLuminance};
    };
    lookup.fill(pack(dataset.colorReferences.global));
    for (const auto& item : dataset.colorReferences.masks) lookup[item.first] = pack(item.second);
    return lookup;
}

inline std::array<float, 3> CalculateMatchingGain(
        const std::array<float, 4>& scene, const std::array<float, 4>& dataset,
        float minTint, float maxTint, float minExposure, float maxExposure) {
    std::array<float, 3> gain{};
    const float exposure = std::clamp(scene[3] / std::max(dataset[3], 0.001f),
        minExposure, maxExposure);
    for (int channel = 0; channel < 3; ++channel) {
        const float tint = std::clamp(scene[channel] / std::max(dataset[channel], 0.001f),
            minTint, maxTint);
        gain[channel] = tint * exposure;
    }
    return gain;
}
