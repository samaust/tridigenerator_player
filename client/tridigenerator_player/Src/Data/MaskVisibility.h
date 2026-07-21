#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

struct MaskVisibilityEntry {
    uint8_t id = 0;
    std::string label;
};

class MaskVisibility {
public:
    MaskVisibility();
    explicit MaskVisibility(const std::unordered_map<uint8_t, std::string>& labels);

    void Reset(const std::unordered_map<uint8_t, std::string>& labels);
    const std::vector<MaskVisibilityEntry>& Entries() const { return entries_; }
    bool IsVisible(uint8_t id) const { return visibility_[id] != 0; }
    void SetVisible(uint8_t id, bool visible) { visibility_[id] = visible ? 1 : 0; }
    void ShowAll();
    void HideAll();
    int* ShaderValues() { return visibility_.data(); }
    const int* ShaderValues() const { return visibility_.data(); }

private:
    std::array<int, 256> visibility_{};
    std::vector<MaskVisibilityEntry> entries_;
};
