#include "Components/MeshDetailSettings.h"

#include <cassert>

int main() {
    assert(MeshDetailSettings{}.divisor == 2);
    for (int divisor = 1; divisor <= 4; ++divisor) {
        assert(MeshDetailControl::IsValidDivisor(divisor));
        assert(MeshDetailControl::SanitizeDivisor(divisor) == divisor);
    }
    assert(!MeshDetailControl::IsValidDivisor(0));
    assert(!MeshDetailControl::IsValidDivisor(5));
    assert(MeshDetailControl::SanitizeDivisor(0) == 2);
    assert(MeshDetailControl::SanitizeDivisor(99) == 2);

    assert(MeshDetailControl::ReducedDimension(1920, 1) == 1920);
    assert(MeshDetailControl::ReducedDimension(1920, 2) == 960);
    assert(MeshDetailControl::ReducedDimension(1920, 3) == 640);
    assert(MeshDetailControl::ReducedDimension(1920, 4) == 480);
    assert(MeshDetailControl::ReducedDimension(1081, 2) == 541);
    assert(MeshDetailControl::ReducedDimension(1081, 3) == 361);
    assert(MeshDetailControl::ReducedDimension(1, 4) == 2);
    assert(MeshDetailControl::ReducedDimension(0, 4) == 2);

    assert(MeshDetailControl::VertexCount(1920, 1080, 1) == 2073600);
    assert(MeshDetailControl::VertexCount(1920, 1080, 2) == 518400);
    assert(MeshDetailControl::VertexCount(1920, 1080, 3) == 230400);
    assert(MeshDetailControl::VertexCount(1920, 1080, 4) == 129600);
    return 0;
}
