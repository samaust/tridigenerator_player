#include "Components/InteractableComponent.h"
#include "Components/TransformComponent.h"
#include "Systems/ScaleControl.h"

#include <cassert>
#include <cmath>

namespace {
bool Near(float a, float b, float epsilon = 1.0e-4f) {
    return std::abs(a - b) <= epsilon;
}
}

int main() {
    InteractableComponent interactable;
    TransformComponent transform;
    assert(interactable.scaleLocked);
    assert(transform.modelScale.x == 1.0f && transform.modelScale.y == 1.0f &&
        transform.modelScale.z == 1.0f);

    assert(Near(ScaleControl::ToLogarithmicPosition(0.01f), -2.0f));
    assert(Near(ScaleControl::ToLogarithmicPosition(1.0f), 0.0f));
    assert(Near(ScaleControl::ToLogarithmicPosition(100.0f), 2.0f));
    assert(Near(ScaleControl::FromLogarithmicPosition(-2.0f), 0.01f));
    assert(Near(ScaleControl::FromLogarithmicPosition(0.0f), 1.0f));
    assert(Near(ScaleControl::FromLogarithmicPosition(2.0f), 100.0f));

    const float steppedUp = ScaleControl::StepLogarithmically(
        1.0f, 1, false, 0.01f, 100.0f);
    assert(steppedUp > 1.05f && steppedUp < 1.07f);
    const float steppedBack = ScaleControl::StepLogarithmically(
        steppedUp, -1, false, 0.01f, 100.0f);
    assert(Near(steppedBack, 1.0f));
    assert(ScaleControl::StepLogarithmically(1.0f, 1, true, 0.01f, 100.0f) == 1.0f);
    assert(ScaleControl::StepLogarithmically(100.0f, 1, false, 0.01f, 100.0f) == 100.0f);
    assert(Near(ScaleControl::StepLogarithmically(
        0.01f, -1, false, 0.01f, 100.0f), 0.01f));

    assert(ScaleControl::ResolveGestureScale(2.0f, 1.0f, true, 0.01f, 100.0f) == 1.0f);
    assert(ScaleControl::ResolveGestureScale(2.0f, 1.0f, false, 0.01f, 100.0f) == 2.0f);
    assert(ScaleControl::ResolveGestureScale(200.0f, 1.0f, false, 0.01f, 100.0f) == 100.0f);
    assert(ScaleControl::NeedsRebaseline(true, false));
    assert(!ScaleControl::NeedsRebaseline(true, true));

    // Reset is an explicit operation and remains valid regardless of lock state.
    assert(ScaleControl::Clamp(1.0f, 0.01f, 100.0f) == 1.0f);
    return 0;
}
