#include "Components/MeshDetailSettings.h"
#include "Systems/DepthTextureResize.h"

#include <cassert>
#include <vector>

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

    assert(DepthTextureResize::SourceCoordinate(0, 5, 3) == 0);
    assert(DepthTextureResize::SourceCoordinate(1, 5, 3) == 2);
    assert(DepthTextureResize::SourceCoordinate(2, 5, 3) == 4);
    const std::vector<uint16_t> source{
        0, 1, 2, 3, 4,
        5, 6, 7, 8, 9,
        10, 11, 12, 13, 14};
    std::vector<uint16_t> resized;
    assert(DepthTextureResize::Nearest(source, 5, 3, 3, 2, resized));
    assert((resized == std::vector<uint16_t>{0, 2, 4, 10, 12, 14}));
    assert(!DepthTextureResize::Nearest(source, 0, 3, 3, 2, resized));
    assert(resized.empty());
    return 0;
}
