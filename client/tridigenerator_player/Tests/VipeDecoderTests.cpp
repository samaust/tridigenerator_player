#include "Videos/WebmInMemoryDemuxer.h"

#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

int main() {
    std::ifstream input(VIPE_TEST_VIDEO, std::ios::binary);
    if (!input) {
        std::cerr << "Could not open test video: " << VIPE_TEST_VIDEO << '\n';
        return 1;
    }
    const std::vector<char> raw{
        std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    const std::vector<uint8_t> bytes(raw.begin(), raw.end());
    WebmInMemoryDemuxer decoder(bytes);
    std::string error;
    if (!decoder.init(&error)) {
        std::cerr << error << '\n';
        return 1;
    }
    for (int index = 0; index < 2; ++index) {
        VideoFrame frame;
        if (!decoder.decode_next_frame(frame)) {
            std::cerr << "Failed to decode complete frame " << index << '\n';
            return 1;
        }
        if (frame.frameIndex != index || frame.textureYWidth != 1280 ||
            frame.textureYHeight != 720 || frame.textureAlphaData.size() != 1280u * 720u ||
            frame.textureDepthData.size() != 1280u * 720u) {
            std::cerr << "Decoded frame metadata is not synchronized at " << index << '\n';
            return 1;
        }
    }
    if (!decoder.seek_to_start()) {
        std::cerr << "Seek to start failed\n";
        return 1;
    }
    VideoFrame looped;
    if (!decoder.decode_next_frame(looped) || looped.frameIndex != 0) {
        std::cerr << "Frame index did not reset after seek\n";
        return 1;
    }
    return 0;
}
