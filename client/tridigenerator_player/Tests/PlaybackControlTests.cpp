#include "Components/FrameLoaderComponent.h"
#include "Data/PlaybackControl.h"

#include <cassert>

int main() {
    FrameLoaderComponent loader;
    assert(!loader.paused);
    assert(ShouldConsumePlaybackFrame(loader.paused));
    loader.paused = true;
    assert(!ShouldConsumePlaybackFrame(loader.paused));

    constexpr uint32_t buttonA = 1u << 3;
    assert(ButtonPressedThisFrame(buttonA, 0, buttonA));
    assert(!ButtonPressedThisFrame(buttonA, buttonA, buttonA));
    assert(!ButtonPressedThisFrame(0, buttonA, buttonA));

    assert(PlaybackDeadlineAfterPauseChange(true, false, 12.0, 3.0) == 12.0);
    assert(PlaybackDeadlineAfterPauseChange(false, true, 12.0, 3.0) == 3.0);
    assert(PlaybackDeadlineAfterPauseChange(false, false, 12.0, 3.0) == 3.0);
    return 0;
}
