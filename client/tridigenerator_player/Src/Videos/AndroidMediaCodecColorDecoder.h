#pragma once

#if defined(__ANDROID__)

#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>

extern "C" {
#include <libavutil/rational.h>
}

struct AVCodecParameters;
struct AVPacket;
struct AImage;
struct AImageReader;
struct AMediaCodec;
struct ANativeWindow;

class AndroidMediaCodecColorDecoder {
public:
    AndroidMediaCodecColorDecoder();
    ~AndroidMediaCodecColorDecoder();

    bool Initialize(
        const AVCodecParameters* parameters,
        AVRational timeBase,
        double sourceFps,
        bool lowLatency,
        std::string& error);
    bool QueuePacket(const AVPacket* packet);
    bool TryGetFrame(
        std::shared_ptr<void>& image,
        int64_t& timestampUs,
        double& outputWaitMilliseconds);
    void Flush();

    const std::string& DecoderName() const { return decoderName_; }
    bool IsHardware() const { return codec_ != nullptr; }

private:
    static void OnImageAvailable(void* context, AImageReader* reader);
    bool IsVendorHardwareName(const std::string& name) const;
    void Reset();

    AMediaCodec* codec_ = nullptr;
    AImageReader* reader_ = nullptr;
    ANativeWindow* window_ = nullptr;
    AVRational timeBase_{1, 1000000};
    std::string decoderName_;
    bool imageAvailable_ = false;
    std::deque<int64_t> presentedTimestampsUs_;
    std::shared_ptr<std::atomic<int>> acquiredImageCount_;
    void* imageMutex_ = nullptr;
    void* imageCondition_ = nullptr;
};

#endif
