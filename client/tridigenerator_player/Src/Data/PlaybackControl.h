#pragma once

#include <cstdint>

inline bool ButtonPressedThisFrame(uint32_t buttons, uint32_t lastButtons, uint32_t mask) {
    return (buttons & mask) != 0 && (lastButtons & mask) == 0;
}

inline bool ShouldConsumePlaybackFrame(bool paused) {
    return !paused;
}

inline double PlaybackDeadlineAfterPauseChange(
        bool wasPaused, bool paused, double nowSeconds, double currentDeadline) {
    return wasPaused && !paused ? nowSeconds : currentDeadline;
}

