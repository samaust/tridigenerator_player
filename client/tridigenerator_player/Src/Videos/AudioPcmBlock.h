#pragma once

#include <cstdint>
#include <vector>

struct AudioPcmBlock {
    std::vector<float> samples;
    int sampleRate = 0;
    int channels = 2;
    int64_t timestampUs = 0;
};
