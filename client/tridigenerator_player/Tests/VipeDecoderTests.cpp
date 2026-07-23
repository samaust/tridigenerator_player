#include "Videos/WebmInMemoryDemuxer.h"

#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    const char* videoPath = argc > 1 ? argv[1] : VIPE_TEST_VIDEO;
    const bool expectAudio = argc > 2 && std::string(argv[2]) == "--expect-audio";
    std::ifstream input(videoPath, std::ios::binary);
    if (!input) {
        std::cerr << "Could not open test video: " << videoPath << '\n';
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
    if (decoder.has_audio() != expectAudio) {
        std::cerr << "Optional audio discovery did not match expectation\n";
        return 1;
    }
    bool decodedAudio = false;
    for (int index = 0; index < 2; ++index) {
        VideoFrame frame;
        if (!decoder.decode_next_frame(frame)) {
            std::cerr << "Failed to decode complete frame " << index << '\n';
            return 1;
        }
        const size_t pixelCount =
            static_cast<size_t>(decoder.width()) * static_cast<size_t>(decoder.height());
        if (frame.frameIndex != index ||
            frame.textureYWidth != static_cast<uint32_t>(decoder.width()) ||
            frame.textureYHeight != static_cast<uint32_t>(decoder.height()) ||
            frame.textureAlphaData.size() != pixelCount ||
            frame.textureDepthData.size() != pixelCount) {
            std::cerr << "Decoded frame metadata is not synchronized at " << index << '\n';
            return 1;
        }
        for (const AudioPcmBlock& block : decoder.take_audio_blocks()) {
            decodedAudio = decodedAudio || (
                block.sampleRate == 48000 && block.channels == 2 &&
                !block.samples.empty());
        }
    }
    if (expectAudio && !decodedAudio) {
        std::cerr << "Audio stream did not produce stereo PCM\n";
        return 1;
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
