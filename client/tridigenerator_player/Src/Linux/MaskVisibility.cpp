#include "MaskVisibility.h"

#include <algorithm>

MaskVisibility::MaskVisibility(const std::unordered_map<uint8_t, std::string>& labels) {
    ShowAll();
    entries_.reserve(labels.size());
    for (const auto& [id, label] : labels) entries_.push_back({id, label});
    std::sort(entries_.begin(), entries_.end(), [](const auto& left, const auto& right) {
        return left.id < right.id;
    });
}

void MaskVisibility::ShowAll() {
    visibility_.fill(1);
}

void MaskVisibility::HideAll() {
    visibility_.fill(0);
}
