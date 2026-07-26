#pragma once

#include <algorithm>
#include <cstddef>

struct MeshDetailSettings {
    int divisor = 2;
};

namespace MeshDetailControl {

inline bool IsValidDivisor(int divisor) {
    return divisor >= 1 && divisor <= 4;
}

inline int SanitizeDivisor(int divisor) {
    return IsValidDivisor(divisor) ? divisor : MeshDetailSettings{}.divisor;
}

inline int ReducedDimension(int sourceDimension, int divisor) {
    divisor = SanitizeDivisor(divisor);
    return std::max(2, (std::max(0, sourceDimension) + divisor - 1) / divisor);
}

inline std::size_t VertexCount(int sourceWidth, int sourceHeight, int divisor) {
    return static_cast<std::size_t>(ReducedDimension(sourceWidth, divisor)) *
        static_cast<std::size_t>(ReducedDimension(sourceHeight, divisor));
}

inline const char* DisplayName(int divisor) {
    switch (divisor) {
        case 1: return "Full";
        case 2: return "1/2";
        case 3: return "1/3";
        case 4: return "1/4";
        default: return "1/2";
    }
}

} // namespace MeshDetailControl
