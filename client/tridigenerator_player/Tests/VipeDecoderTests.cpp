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
    VideoFrame frame;
    size_t yCapacity = 0;
    size_t alphaCapacity = 0;
    size_t depthCapacity = 0;
    for (int index = 0; index < 16; ++index) {
        DecodeInvocationTiming timing;
        if (!decoder.decode_next_frame(frame, &timing)) {
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
        if (!frame.colorPlaneOwner || !frame.textureYData.empty() ||
            !frame.textureUData.empty() || !frame.textureVData.empty() ||
            !frame.colorPlaneViews[0].data ||
            frame.colorPlaneViews[0].stride <
                static_cast<int>(frame.colorPlaneViews[0].width) ||
            frame.colorPlaneViews[1].stride <
                static_cast<int>(frame.colorPlaneViews[1].width) ||
            frame.colorPlaneViews[2].stride <
                static_cast<int>(frame.colorPlaneViews[2].width)) {
            std::cerr << "dav1d planes were not retained with valid strides\n";
            return 1;
        }
        if (timing.colorCopyMilliseconds != 0.0) {
            std::cerr << "dav1d performed an unexpected decoder-thread color copy\n";
            return 1;
        }
        if (timing.totalMilliseconds <= 0.0 ||
            timing.colorCodecMilliseconds < 0.0 ||
            timing.alphaCodecMilliseconds < 0.0 ||
            timing.depthCodecMilliseconds < 0.0) {
            std::cerr << "Decode stage timing was not populated\n";
            return 1;
        }
        if (index == 0) {
            yCapacity = frame.textureYData.capacity();
            alphaCapacity = frame.textureAlphaData.capacity();
            depthCapacity = frame.textureDepthData.capacity();
        } else if (frame.textureYData.capacity() != yCapacity ||
                   frame.textureAlphaData.capacity() != alphaCapacity ||
                   frame.textureDepthData.capacity() != depthCapacity) {
            std::cerr << "Decoded frame storage reallocated in steady state\n";
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
    int decodedFrames = 16;
    while (decoder.decode_next_frame(frame)) {
        if (frame.frameIndex != decodedFrames) {
            std::cerr << "Frame order changed while draining the stream\n";
            return 1;
        }
        ++decodedFrames;
    }
    if (decodedFrames != 122) {
        std::cerr << "Decoder produced " << decodedFrames
                  << " frames before EOS; expected 122\n";
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
