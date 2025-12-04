#pragma once
#include <cstdint>
#include <vector>
#include <string>
#include <functional>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
}

#include <dav1d/dav1d.h>
#include <dav1d/data.h>
#include <dav1d/picture.h>

/**
 * Struct returned for each decoded frame.
 * All planes are tightly packed (row stride == width for each plane).
 */
struct VideoFrame {
    std::vector<uint8_t> textureYData;
    uint32_t textureYWidth = 0;
    uint32_t textureYHeight = 0;

    std::vector<uint8_t> textureUData;
    uint32_t textureUWidth = 0;
    uint32_t textureUHeight = 0;

    std::vector<uint8_t> textureVData;
    uint32_t textureVWidth = 0;
    uint32_t textureVHeight = 0;

    int64_t ts_us = 0;

    // Ownership: these internal buffers belong to WebmInMemoryDemuxer
    // and remain valid until the next call to decode_next_frame().
};


/**
 * A streaming WebM → AV1 → YUV420 (8-bit) demuxer/decoder.
 * Operates on an AV1 WebM video stored entirely in memory.
 *
 * Usage:
 *   WebmInMemoryDemuxer demux(blob);
 *   if (!demux.init()) { ...error... }
 *   while (true) {
 *       VideoFrame frame;
 *       if (!demux.decode_next_frame(frame)) {
 *           demux.seek_to_start();
 *           continue;
 *       }
 *       upload_to_gpu(frame);
 *   }
 */
class WebmInMemoryDemuxer {
public:
    explicit WebmInMemoryDemuxer(const std::vector<uint8_t>& blob);
    ~WebmInMemoryDemuxer();

    /**
     * Initialize FFmpeg + dav1d contexts.
     * Must be called once after construction.
     *
     * Returns false on failure (unsupported bit depth, invalid WebM, etc).
     */
    bool init(std::string* error = nullptr);

    /**
     * Decode the next video frame.
     * Returns false on end-of-stream or error.
     * If EOS: user should call seek_to_start() and try again.
     */
    bool decode_next_frame(VideoFrame& outFrame);

    /**
     * Seek to the beginning of the WebM stream (timestamp = 0).
     * Clears decoder state.
     */
    bool seek_to_start();

    /**
     * Returns the codec timebase (AVStream->time_base).
     * Useful for external scheduling.
     */
    AVRational stream_timebase() const { return timeBase_; }

    /**
     * Returns width and height of the coded video.
     */
    int width() const { return width_; }
    int height() const { return height_; }

private:
    // Core initialization steps
    bool init_ffmpeg(std::string* error);
    bool init_dav1d(std::string* error);

    // Pull one packet from FFmpeg
    bool read_packet(AVPacket* pkt);

    // Submit one packet to dav1d
    bool submit_packet_to_dav1d(const AVPacket* pkt);

    // Extract a decoded dav1d picture
    bool get_next_dav1d_picture(VideoFrame& outFrame);

    // Convert Dav1dPicture → tightly packed YUV420 buffers
    bool copy_picture_planes(const Dav1dPicture* pic, VideoFrame& outFrame);

    // Reset decoder state (for seeking)
    void flush_decoders();

private:
    const std::vector<uint8_t>& blob_;

    // FFmpeg members
    AVFormatContext* fmtCtx_ = nullptr;
    AVCodecContext* codecCtx_ = nullptr;
    int videoStreamIndex_ = -1;
    AVRational timeBase_{1, 1000000};

    // dav1d members
    Dav1dContext* dav1dCtx_ = nullptr;
    Dav1dSequenceHeader seqHdr_{};

    // Video parameters
    int width_ = 0;
    int height_ = 0;
    int bitDepth_ = 8;

    // AVIO buffer
    std::vector<uint8_t> avioBuffer_;
    AVIOContext* avioCtx_ = nullptr;

    // Position inside blob_
    int64_t readPos_ = 0;

    // FFmpeg expects static functions with 'void* opaque' parameter.
    static int read_callback(void* opaque, uint8_t* buf, int bufSize);
    static int64_t seek_callback(void* opaque, int64_t offset, int whence);

    // Instance methods that actually implement the logic (no opaque param).
    int read_callback(uint8_t* buf, int bufSize);
    int64_t seek_callback(int64_t offset, int whence);
};
