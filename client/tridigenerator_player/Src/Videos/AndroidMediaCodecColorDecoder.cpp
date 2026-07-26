#include "AndroidMediaCodecColorDecoder.h"

#if defined(__ANDROID__)

#include <algorithm>
#include <chrono>
#include <cctype>
#include <condition_variable>
#include <cstring>
#include <mutex>

#include <android/hardware_buffer.h>
#include <media/NdkImage.h>
#include <media/NdkImageReader.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaError.h>
#include <media/NdkMediaFormat.h>

extern "C" {
#include <libavcodec/codec_par.h>
#include <libavcodec/packet.h>
#include <libavutil/rational.h>
}

namespace {

constexpr const char* kMime = "video/av01";
// The player owns up to eight frame-ring entries concurrently. Leave two
// additional slots for the image being acquired and producer progress.
constexpr int kMaxAcquiredImages = 10;

std::string Lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

} // namespace

AndroidMediaCodecColorDecoder::AndroidMediaCodecColorDecoder()
    : acquiredImageCount_(std::make_shared<std::atomic<int>>(0)),
      imageMutex_(new std::mutex),
      imageCondition_(new std::condition_variable) {}

AndroidMediaCodecColorDecoder::~AndroidMediaCodecColorDecoder() {
    Reset();
    delete static_cast<std::mutex*>(imageMutex_);
    delete static_cast<std::condition_variable*>(imageCondition_);
}

bool AndroidMediaCodecColorDecoder::IsVendorHardwareName(
        const std::string& name) const {
    const std::string lower = Lower(name);
    return lower.find("c2.android") == std::string::npos &&
        lower.find("omx.google") == std::string::npos &&
        lower.find("software") == std::string::npos &&
        lower.find("sw.decoder") == std::string::npos;
}

void AndroidMediaCodecColorDecoder::OnImageAvailable(
        void* context, AImageReader*) {
    auto* self = static_cast<AndroidMediaCodecColorDecoder*>(context);
    {
        std::lock_guard<std::mutex> lock(
            *static_cast<std::mutex*>(self->imageMutex_));
        self->imageAvailable_ = true;
    }
    static_cast<std::condition_variable*>(self->imageCondition_)->notify_one();
}

bool AndroidMediaCodecColorDecoder::Initialize(
        const AVCodecParameters* parameters,
        AVRational timeBase,
        double sourceFps,
        bool lowLatency,
        std::string& error) {
    Reset();
    timeBase_ = timeBase;

    if (AImageReader_newWithUsage(
            parameters->width,
            parameters->height,
            AIMAGE_FORMAT_PRIVATE,
            AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE,
            kMaxAcquiredImages,
            &reader_) != AMEDIA_OK ||
        AImageReader_getWindow(reader_, &window_) != AMEDIA_OK) {
        error = "MediaCodec: could not create private GPU AImageReader";
        Reset();
        return false;
    }
    AImageReader_ImageListener listener{this, &OnImageAvailable};
    AImageReader_setImageListener(reader_, &listener);

    const char* candidates[] = {
        "c2.qti.av1.decoder",
        "OMX.qcom.video.decoder.av1",
    };
    for (const char* candidate : candidates) {
        codec_ = AMediaCodec_createCodecByName(candidate);
        if (codec_) {
            decoderName_ = candidate;
            break;
        }
    }
    if (!codec_ || !IsVendorHardwareName(decoderName_)) {
        error = codec_
            ? "MediaCodec selected a prohibited software decoder: " + decoderName_
            : "No vendor AV1 MediaCodec decoder was found";
        Reset();
        return false;
    }

    AMediaFormat* format = AMediaFormat_new();
    AMediaFormat_setString(format, AMEDIAFORMAT_KEY_MIME, kMime);
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_WIDTH, parameters->width);
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_HEIGHT, parameters->height);
    AMediaFormat_setInt32(format, "priority", 0);
    AMediaFormat_setFloat(
        format, "operating-rate",
        static_cast<float>(sourceFps));
    if (lowLatency) {
        AMediaFormat_setInt32(format, "low-latency", 1);
    }
    if (parameters->extradata && parameters->extradata_size > 0) {
        AMediaFormat_setBuffer(
            format,
            "csd-0",
            parameters->extradata,
            static_cast<size_t>(parameters->extradata_size));
    }
    const media_status_t configured =
        AMediaCodec_configure(codec_, format, window_, nullptr, 0);
    AMediaFormat_delete(format);
    if (configured != AMEDIA_OK || AMediaCodec_start(codec_) != AMEDIA_OK) {
        error = "Vendor AV1 MediaCodec failed to configure/start: " + decoderName_;
        Reset();
        return false;
    }
    return true;
}

bool AndroidMediaCodecColorDecoder::QueuePacket(const AVPacket* packet) {
    if (!codec_ || !packet) return false;
    const ssize_t index = AMediaCodec_dequeueInputBuffer(codec_, 0);
    if (index < 0) return false;
    size_t capacity = 0;
    uint8_t* input = AMediaCodec_getInputBuffer(
        codec_, static_cast<size_t>(index), &capacity);
    if (!input || capacity < static_cast<size_t>(packet->size)) return false;
    std::memcpy(input, packet->data, static_cast<size_t>(packet->size));
    int64_t timestampUs = packet->pts;
    if (timestampUs == AV_NOPTS_VALUE) timestampUs = packet->dts;
    if (timestampUs != AV_NOPTS_VALUE) {
        timestampUs = av_rescale_q(
            timestampUs, timeBase_,
            AVRational{1, 1000000});
    } else {
        timestampUs = 0;
    }
    return AMediaCodec_queueInputBuffer(
        codec_, static_cast<size_t>(index), 0,
        static_cast<size_t>(packet->size), static_cast<uint64_t>(timestampUs),
        0) == AMEDIA_OK;
}

bool AndroidMediaCodecColorDecoder::TryGetFrame(
        std::shared_ptr<void>& image,
        int64_t& timestampUs,
        double& outputWaitMilliseconds) {
    if (!codec_) return false;
    const auto start = std::chrono::steady_clock::now();
    if (presentedTimestampsUs_.empty()) {
        AMediaCodecBufferInfo info{};
        const ssize_t index = AMediaCodec_dequeueOutputBuffer(codec_, &info, 0);
        if (index < 0) return false;
        if (AMediaCodec_releaseOutputBuffer(
                codec_, static_cast<size_t>(index), true) != AMEDIA_OK) {
            return false;
        }
        presentedTimestampsUs_.push_back(info.presentationTimeUs);
    }

    // Surface delivery is asynchronous. Keep the output PTS queued if the
    // corresponding AImage is not ready during this call; dropping that PTS
    // pairs the following image with the wrong alpha/depth frame.
    std::unique_lock<std::mutex> lock(*static_cast<std::mutex*>(imageMutex_));
    if (!imageAvailable_) {
        static_cast<std::condition_variable*>(imageCondition_)->wait_for(
            lock, std::chrono::milliseconds(12), [&] { return imageAvailable_; });
    }
    imageAvailable_ = false;
    lock.unlock();
    if (acquiredImageCount_->load(std::memory_order_acquire) >=
        kMaxAcquiredImages) {
        return false;
    }
    AImage* acquired = nullptr;
    const media_status_t acquireStatus =
        AImageReader_acquireNextImage(reader_, &acquired);
    if (acquireStatus != AMEDIA_OK || !acquired) {
        return false;
    }
    acquiredImageCount_->fetch_add(1, std::memory_order_acq_rel);
    outputWaitMilliseconds =
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - start).count();
    timestampUs = presentedTimestampsUs_.front();
    presentedTimestampsUs_.pop_front();
    const auto leaseCount = acquiredImageCount_;
    image = std::shared_ptr<void>(
        acquired,
        [leaseCount](void* value) {
            AImage_delete(static_cast<AImage*>(value));
            leaseCount->fetch_sub(1, std::memory_order_acq_rel);
        });
    return true;
}

void AndroidMediaCodecColorDecoder::Flush() {
    if (codec_) AMediaCodec_flush(codec_);
    presentedTimestampsUs_.clear();
    std::lock_guard<std::mutex> lock(*static_cast<std::mutex*>(imageMutex_));
    imageAvailable_ = false;
}

void AndroidMediaCodecColorDecoder::Reset() {
    if (codec_) {
        AMediaCodec_stop(codec_);
        AMediaCodec_delete(codec_);
        codec_ = nullptr;
    }
    window_ = nullptr;
    if (reader_) {
        AImageReader_delete(reader_);
        reader_ = nullptr;
    }
    decoderName_.clear();
    presentedTimestampsUs_.clear();
}

#endif
