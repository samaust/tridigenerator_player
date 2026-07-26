#include "Components/FrameLoaderComponent.h"
#include "Data/PlaybackControl.h"

#include <cassert>

int main() {
    FrameLoaderComponent loader;
    assert(!loader.paused.load());
    assert(ShouldConsumePlaybackFrame(loader.paused.load()));
    loader.paused.store(true);
    assert(!ShouldConsumePlaybackFrame(loader.paused.load()));

    assert(ShouldWakeFrameWriter(false, true, 0));
    assert(!ShouldWakeFrameWriter(true, true, 1));
    assert(!ShouldWakeFrameWriter(true, false, 0));
    assert(ShouldWakeFrameWriter(true, false, 1));

    constexpr uint32_t buttonA = 1u << 3;
    assert(ButtonPressedThisFrame(buttonA, 0, buttonA));
    assert(!ButtonPressedThisFrame(buttonA, buttonA, buttonA));
    assert(!ButtonPressedThisFrame(0, buttonA, buttonA));

    assert(PlaybackDeadlineAfterPauseChange(true, false, 12.0, 3.0) == 12.0);
    assert(PlaybackDeadlineAfterPauseChange(false, true, 12.0, 3.0) == 3.0);
    assert(PlaybackDeadlineAfterPauseChange(false, false, 12.0, 3.0) == 3.0);
    return 0;
}
