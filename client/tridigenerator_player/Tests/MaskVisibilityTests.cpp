#include "Linux/MaskVisibility.h"

#include <iostream>
#include <string>
#include <unordered_map>

namespace {

int failures = 0;

void Expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

} // namespace

int main() {
    MaskVisibility visibility({{3, "pet"}, {0, "background"}, {2, "animal"}});
    for (int id = 0; id < 256; ++id) {
        Expect(visibility.IsVisible(static_cast<uint8_t>(id)), "all IDs initially visible");
    }

    const auto& entries = visibility.Entries();
    Expect(entries.size() == 3, "manifest entries retained");
    Expect(entries[0].id == 0 && entries[0].label == "background", "background sorted first");
    Expect(entries[1].id == 2 && entries[1].label == "animal", "animal label retained");
    Expect(entries[2].id == 3 && entries[2].label == "pet", "pet label retained");

    visibility.SetVisible(0, false);
    Expect(!visibility.IsVisible(0), "background can be hidden");
    Expect(visibility.IsVisible(2), "hiding background does not hide animal");
    visibility.SetVisible(0, true);
    Expect(visibility.IsVisible(0), "background can be restored");

    visibility.HideAll();
    for (int id = 0; id < 256; ++id) {
        Expect(!visibility.IsVisible(static_cast<uint8_t>(id)), "HideAll hides every ID");
    }
    visibility.ShowAll();
    for (int id = 0; id < 256; ++id) {
        Expect(visibility.IsVisible(static_cast<uint8_t>(id)), "ShowAll shows every ID");
    }

    return failures == 0 ? 0 : 1;
}
