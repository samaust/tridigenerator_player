#pragma once
#include <cstdint>
#include <vector>
#include <string>
#include <functional>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/avutil.h>
#include <libavutil/pixfmt.h>
}

#include <dav1d/dav1d.h>
#include <dav1d/data.h>
#include <dav1d/picture.h>

#include "Render/VideoFrame.h"


/**
 * @class WebmInMemoryDemuxer
 * @brief Demuxes and decodes a WebM video file held entirely in memory.
 *
 * This class handles demuxing a WebM container, decoding its video streams
 * (AV1 for color, FFV1 for alpha and PNG for depth), and providing frames
 * for rendering.
 * It uses FFmpeg for demuxing and decoding FFV1/PNG streams, and
 * dav1d for AV1 decoding.
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
    AVRational timeBase_{1, 1000000};

    // --- STREAM INDICES ---
    int colorStreamIndex_ = -1;
    int alphaStreamIndex_ = -1;
    int depthStreamIndex_ = -1;

    // --- DECODER CONTEXTS ---
    // dav1d for AV1 color stream
    Dav1dContext* dav1dCtx_ = nullptr;
    // FFmpeg AVCodec for FFV1 alpha stream
    AVCodecContext* alphaCodecCtx_ = nullptr;
    // FFmpeg AVCodec for PNG depth stream
    AVCodecContext* depthCodecCtx_ = nullptr;
    SwsContext* swsCtx_ = nullptr;

    // Video parameters
    int width_ = 0;
    int height_ = 0;
    bool colorRangeKnown_ = false;
    bool colorFullRange_ = false;

    // AVIO buffer
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
