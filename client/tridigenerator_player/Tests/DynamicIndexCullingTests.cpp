#include "Data/DynamicIndexCulling.h"

#include <cassert>
#include <cstdint>
#include <vector>

namespace {

MaskVisibilitySnapshot Visibility(bool visible) {
    MaskVisibilitySnapshot snapshot;
    snapshot.words.fill(visible ? ~std::uint64_t{0} : std::uint64_t{0});
    snapshot.version = 7;
    return snapshot;
}

DynamicCellCullingData Prepare(
        const std::vector<std::uint8_t>& masks,
        std::uint32_t maskWidth,
        std::uint32_t maskHeight,
        const std::vector<std::uint16_t>& depth,
        std::uint32_t meshWidth,
        std::uint32_t meshHeight,
        const MaskVisibilitySnapshot& visibility) {
    DynamicCellCullingData output;
    assert(DynamicIndexCulling::Prepare(
        masks, maskWidth, maskHeight,
        depth, meshWidth, meshHeight,
        visibility, output));
    return output;
}

} // namespace

int main() {
    {
        MaskVisibilityPublisher publisher;
        const auto initial = publisher.Snapshot();
        assert(initial.IsVisible(0));
        assert(initial.IsVisible(255));
        int values[256]{};
        values[7] = 1;
        publisher.Publish(values);
        const auto changed = publisher.Snapshot();
        assert(changed.version == initial.version + 1);
        assert(changed.IsVisible(7));
        assert(!changed.IsVisible(6));
    }
    {
        const auto output = Prepare(
            std::vector<std::uint8_t>(4, 1), 2, 2,
            std::vector<std::uint16_t>(4, 100), 2, 2,
            Visibility(true));
        assert(output.retainedCells == std::vector<std::uint8_t>{1});
        assert(output.retainedCellCount == 1);
        assert(output.rejectedCellCount == 0);
        assert(output.mixedCellCount == 0);
        assert(output.Matches(2, 2, 7));
    }
    {
        const auto output = Prepare(
            std::vector<std::uint8_t>(4, 1), 2, 2,
            std::vector<std::uint16_t>(4, 0), 2, 2,
            Visibility(true));
        assert(output.retainedCellCount == 0);
        assert(output.rejectedCellCount == 1);
    }
    {
        const auto output = Prepare(
            std::vector<std::uint8_t>(4, 1), 2, 2,
            std::vector<std::uint16_t>(4, 100), 2, 2,
            Visibility(false));
        assert(output.retainedCellCount == 0);
        assert(output.rejectedCellCount == 1);
    }
    {
        auto visibility = Visibility(false);
        visibility.words[0] |= std::uint64_t{1} << 1;
        std::vector<std::uint8_t> masks(16, 2);
        masks[5] = 1;
        std::vector<std::uint16_t> depth(16, 0);
        depth[10] = 100;
        const auto output = Prepare(
            masks, 4, 4, depth, 4, 4, visibility);
        // Visible mask and valid depth need not be at the same texel. Keeping
        // their conservatively overlapping cells cannot remove visible output.
        assert(output.retainedCellCount > 0);
        assert(output.mixedCellCount == output.retainedCellCount);
    }
    {
        auto visibility = Visibility(false);
        visibility.words[0] |= std::uint64_t{1} << 1;
        std::vector<std::uint8_t> masks(81, 2);
        masks[1] = 1; // top mask row
        std::vector<std::uint16_t> depth(9, 100);
        const auto output = Prepare(
            masks, 9, 9, depth, 3, 3, visibility);
        // Mesh UV Y is flipped, so top texture content retains the bottom row.
        assert(output.retainedCells[2] != 0 || output.retainedCells[3] != 0);
    }
    {
        DynamicCellCullingData output;
        assert(!DynamicIndexCulling::Prepare(
            {}, 0, 0, {}, 1, 1, Visibility(true), output));
        assert(output.retainedCells.empty());
    }
    return 0;
}
