// WebmInMemoryDemuxer.cpp
#include "WebmInMemoryDemuxer.h"

#include <stdexcept>
#include <cstring>
#include <algorithm>
#include <iostream>

#include <android/log.h>
#define LOG_TAG "WebmInMemoryDemuxer"
#define ALOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define ALOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/mem.h>
#include <libavutil/pixdesc.h>
#include <libavcodec/avcodec.h>

#include <dav1d/dav1d.h>
#include <dav1d/data.h>
#include <dav1d/picture.h>
}

// ---------- Helpers ----------
static void throw_if_ffmpeg_err(int ret, const char* ctx) {
    if (ret >= 0) return;
    char buf[256] = {0};
    av_strerror(ret, buf, sizeof(buf));
    throw std::runtime_error(std::string(ctx) + ": " + buf);
}

static void dav1d_free_avpacket_cb(const uint8_t* /*buf*/, void* cookie) {
    if (!cookie) return;
    AVPacket* pkt_ref = reinterpret_cast<AVPacket*>(cookie);
    av_packet_unref(pkt_ref);
    av_packet_free(&pkt_ref);
}

static inline int64_t pts_to_us(int64_t pts, AVRational tb) {
    if (pts == AV_NOPTS_VALUE) return AV_NOPTS_VALUE;
    AVRational us_tb = {1, 1000000};
    return av_rescale_q(pts, tb, us_tb);
}

// ---------- Static callback forwarders ----------
int WebmInMemoryDemuxer::read_callback(void* opaque, uint8_t* buf, int bufSize) {
    auto* self = reinterpret_cast<WebmInMemoryDemuxer*>(opaque);
    return self->read_callback(buf, bufSize);
}

int64_t WebmInMemoryDemuxer::seek_callback(void* opaque, int64_t offset, int whence) {
    auto* self = reinterpret_cast<WebmInMemoryDemuxer*>(opaque);
    return self->seek_callback(offset, whence);
}

// ---------- Member callback implementations ----------
int WebmInMemoryDemuxer::read_callback(uint8_t* buf, int bufSize) {
    const int64_t remaining = static_cast<int64_t>(blob_.size()) - readPos_;
    if (remaining <= 0) return AVERROR_EOF;
    const int toRead = static_cast<int>(std::min<int64_t>(bufSize, remaining));
    std::memcpy(buf, blob_.data() + readPos_, toRead);
    readPos_ += toRead;
    return toRead;
}

int64_t WebmInMemoryDemuxer::seek_callback(int64_t offset, int whence) {
    if (whence == AVSEEK_SIZE) {
        return static_cast<int64_t>(blob_.size());
    }

    int64_t newPos = offset;
    if (whence == SEEK_CUR) newPos = readPos_ + offset;
    else if (whence == SEEK_END) newPos = static_cast<int64_t>(blob_.size()) + offset;

    if (newPos < 0 || newPos > static_cast<int64_t>(blob_.size())) return -1;
    readPos_ = newPos;
    return newPos;
}

// ---------- Construction / destruction ----------
WebmInMemoryDemuxer::WebmInMemoryDemuxer(const std::vector<uint8_t>& blob)
        : blob_(blob) {
    // members initialized in init()
}

WebmInMemoryDemuxer::~WebmInMemoryDemuxer() {
    // Cleanup
    if (dav1dCtx_) {
        dav1d_close(&dav1dCtx_);
        dav1dCtx_ = nullptr;
    }

    if (fmtCtx_) {
        avformat_close_input(&fmtCtx_);
        fmtCtx_ = nullptr;
    }

    if (avioCtx_) {
        // avioCtx_->buffer was allocated with av_malloc in init(); free it.
        if (avioCtx_->buffer) av_free(avioCtx_->buffer);
        avio_context_free(&avioCtx_);
        avioCtx_ = nullptr;
    }

    avformat_network_deinit();
}

// ---------- init ----------
bool WebmInMemoryDemuxer::init(std::string* error) {
    try {
        // sanity
        if (blob_.empty()) throw std::runtime_error("input blob is empty");

        av_log_set_level(AV_LOG_ERROR);
        avformat_network_init();

        // create AVIOContext with this as opaque
        readPos_ = 0;
        const int bufSz = 1 << 16; // 64KB
        uint8_t* avioBuf = static_cast<uint8_t*>(av_malloc(bufSz + AV_INPUT_BUFFER_PADDING_SIZE));
        if (!avioBuf) throw std::bad_alloc();

        avioCtx_ = avio_alloc_context(
                avioBuf,
                bufSz,
                0,                  // read-only
                this,               // opaque
                &WebmInMemoryDemuxer::read_callback,
                nullptr,
                &WebmInMemoryDemuxer::seek_callback
        );
        if (!avioCtx_) {
            av_free(avioBuf);
            throw std::runtime_error("avio_alloc_context failed");
        }

        fmtCtx_ = avformat_alloc_context();
        if (!fmtCtx_) throw std::runtime_error("avformat_alloc_context failed");
        fmtCtx_->pb = avioCtx_;
        fmtCtx_->flags |= AVFMT_FLAG_CUSTOM_IO;

        throw_if_ffmpeg_err(avformat_open_input(&fmtCtx_, nullptr, nullptr, nullptr), "avformat_open_input");
        throw_if_ffmpeg_err(avformat_find_stream_info(fmtCtx_, nullptr), "avformat_find_stream_info");

        // --- STREAM FINDING LOGIC ---
        colorStreamIndex_ = -1;
        alphaStreamIndex_ = -1;
        depthStreamIndex_ = -1;

        for (unsigned i = 0; i < fmtCtx_->nb_streams; ++i) {
            AVStream* stream = fmtCtx_->streams[i];
            AVCodecParameters* cp = stream->codecpar;
            if (!cp) continue;

            if (cp->codec_type == AVMEDIA_TYPE_VIDEO) {
                if (cp->codec_id == AV_CODEC_ID_AV1 && colorStreamIndex_ == -1) {
                    colorStreamIndex_ = (int)i;
                    ALOGI("Found AV1 color stream at index %d", i);
                } else if (cp->codec_id == AV_CODEC_ID_FFV1 && cp->format == AV_PIX_FMT_GRAY8 && alphaStreamIndex_ == -1) {
                    alphaStreamIndex_ = (int)i;
                    ALOGI("Found FFV1 alpha stream (gray8) at index %d", i);
                } else if (cp->codec_id == AV_CODEC_ID_PNG && cp->format == AV_PIX_FMT_GRAY16BE && depthStreamIndex_ == -1) {
                    depthStreamIndex_ = (int)i;
                    ALOGI("Found FFV1 depth stream (gray16be) at index %d", i);
                }
            }
        }

        if (colorStreamIndex_ < 0 || alphaStreamIndex_ < 0 || depthStreamIndex_ < 0) {
            throw std::runtime_error("Failed to find all required streams (color, alpha, depth)");
        }

        // --- INITIALIZE DECODERS ---
        // dav1d for color
        Dav1dSettings s;
        dav1d_default_settings(&s);
        s.n_threads = 0; // auto
        if (dav1d_open(&dav1dCtx_, &s) < 0) throw std::runtime_error("dav1d_open failed");

        // FFmpeg decoder for alpha stream
        AVStream* alphaStream = fmtCtx_->streams[alphaStreamIndex_];
        const AVCodec* alphaCodec = avcodec_find_decoder(alphaStream->codecpar->codec_id);
        if (!alphaCodec) throw std::runtime_error("Could not find FFV1 decoder for alpha");
        alphaCodecCtx_ = avcodec_alloc_context3(alphaCodec);
        throw_if_ffmpeg_err(avcodec_parameters_to_context(alphaCodecCtx_, alphaStream->codecpar), "alpha avcodec_parameters_to_context");
        throw_if_ffmpeg_err(avcodec_open2(alphaCodecCtx_, alphaCodec, nullptr), "alpha avcodec_open2");

        // FFmpeg decoder for depth stream
        AVStream* depthStream = fmtCtx_->streams[depthStreamIndex_];
        const AVCodec* depthCodec = avcodec_find_decoder(depthStream->codecpar->codec_id);
        if (!depthCodec) throw std::runtime_error("Could not find PNG decoder for depth");
        depthCodecCtx_ = avcodec_alloc_context3(depthCodec);
        throw_if_ffmpeg_err(avcodec_parameters_to_context(depthCodecCtx_, depthStream->codecpar), "depth avcodec_parameters_to_context");
        throw_if_ffmpeg_err(avcodec_open2(depthCodecCtx_, depthCodec, nullptr), "depth avcodec_open2");

        // 2. Initialize SwScaler for conversion
        swsCtx_ = sws_getContext(
                depthCodecCtx_->width, depthCodecCtx_->height, depthCodecCtx_->pix_fmt, // Source info
                depthCodecCtx_->width, depthCodecCtx_->height, AV_PIX_FMT_GRAY16LE, // Target info
                SWS_POINT, // SWS_POINT ensures a lossless conversion (like byte swapping)
                NULL, NULL, NULL
        );
        if (!swsCtx_) throw std::runtime_error("Could not initialize SwsContext");

        // Set main timebase/width/height from the color stream
        AVStream* vst = fmtCtx_->streams[colorStreamIndex_];
        timeBase_ = vst->time_base;
        width_ = fmtCtx_->streams[colorStreamIndex_]->codecpar->width;
        height_ = fmtCtx_->streams[colorStreamIndex_]->codecpar->height;

        // ready
        return true;
    } catch (const std::exception& ex) {
        if (error) *error = ex.what();
        // cleanup partial state
        if (dav1dCtx_) { dav1d_close(&dav1dCtx_); dav1dCtx_ = nullptr; }
        if (fmtCtx_) { avformat_close_input(&fmtCtx_); fmtCtx_ = nullptr; }
        if (avioCtx_) {
            if (avioCtx_->buffer) av_free(avioCtx_->buffer);
            avio_context_free(&avioCtx_);
            avioCtx_ = nullptr;
        }
        avformat_network_deinit();
        return false;
    }
}

// ---------- seek_to_start ----------
bool WebmInMemoryDemuxer::seek_to_start() {
    if (!fmtCtx_) return false;
    AVStream* st = fmtCtx_->streams[colorStreamIndex_];
    AVRational us_tb = {1, 1000000};
    int64_t target_pts = av_rescale_q(0, us_tb, st->time_base);

    int r = avformat_seek_file(fmtCtx_, colorStreamIndex_, INT64_MIN, target_pts, INT64_MAX, AVSEEK_FLAG_BACKWARD);
    if (r < 0) {
        r = av_seek_frame(fmtCtx_, colorStreamIndex_, target_pts, AVSEEK_FLAG_BACKWARD);
        if (r < 0) {
            char buf[256]; av_strerror(r, buf, sizeof(buf));
            std::cerr << "seek failed: " << buf << "\n";
            return false;
        }
    }

    flush_decoders();
    return true;
}

// ---------- read packet (wrapper) ----------
bool WebmInMemoryDemuxer::read_packet(AVPacket* pkt) {
    int r = av_read_frame(fmtCtx_, pkt);
    if (r == AVERROR_EOF) {
        return false;
    }
    throw_if_ffmpeg_err(r, "av_read_frame");
    return true;
}

// ---------- submit packet to dav1d ----------
bool WebmInMemoryDemuxer::submit_packet_to_dav1d(const AVPacket* pkt) {
    if (!pkt || pkt->size <= 0) return true; // nothing to do

    // create a reference packet that will be freed by the dav1d free callback
    AVPacket* pkt_ref = av_packet_alloc();
    if (!pkt_ref) throw std::bad_alloc();
    int r = av_packet_ref(pkt_ref, pkt);
    if (r < 0) {
        av_packet_free(&pkt_ref);
        throw std::runtime_error("av_packet_ref failed");
    }

    Dav1dData d;
    memset(&d, 0, sizeof(d));
    int w = dav1d_data_wrap(&d, pkt_ref->data, (size_t)pkt_ref->size, dav1d_free_avpacket_cb, pkt_ref);
    if (w < 0) {
        // wrap failed -> free pkt_ref ourselves
        av_packet_unref(pkt_ref);
        av_packet_free(&pkt_ref);
        throw std::runtime_error("dav1d_data_wrap failed");
    }

    // convert pts to microseconds and store in data.m.timestamp
    AVStream* st = fmtCtx_->streams[colorStreamIndex_];
    int64_t ts_us = pts_to_us(pkt_ref->pts, st->time_base);
    if (ts_us == AV_NOPTS_VALUE) ts_us = pts_to_us(pkt_ref->dts, st->time_base);
    d.m.timestamp = ts_us;

    int s = dav1d_send_data(dav1dCtx_, &d);
    if (s < 0) {
        // dav1d_send_data failed -> we must release our pkt_ref because dav1d won't call the callback
        dav1d_data_unref(&d);
        throw std::runtime_error("dav1d_send_data failed");
    }

    // Success: pkt_ref will be freed by dav1d when it releases the data.
    return true;
}

// ---------- get next dav1d picture and populate outFrame ----------
bool WebmInMemoryDemuxer::get_next_dav1d_picture(VideoFrame& outFrame) {
    Dav1dPicture pic;
    memset(&pic, 0, sizeof(pic));
    int r = dav1d_get_picture(dav1dCtx_, &pic);
    if (r == 0) {
        // picture available
        // Check bit depth (bytes per component)
        int bpc = 1;
        // prefer pic.p.bpc if present
        bpc = pic.p.bpc;
        if (bpc != 8 && bpc != 1) {
            // dav1d sometimes uses bpc==1 for 8-bit, but defensively check != 8
            if (bpc != 1) {
                dav1d_picture_unref(&pic);
                throw std::runtime_error("Unsupported bit depth (only 8-bit supported)");
            }
        }

        // Determine layout/subsampling
        int ss_hor = 0, ss_ver = 0;
        switch (pic.p.layout) {
            case DAV1D_PIXEL_LAYOUT_I420: ss_hor = 1; ss_ver = 1; break; // 4:2:0
            case DAV1D_PIXEL_LAYOUT_I422: ss_hor = 1; ss_ver = 0; break; // 4:2:2
            case DAV1D_PIXEL_LAYOUT_I444: ss_hor = 0; ss_ver = 0; break; // 4:4:4
            default:
                dav1d_picture_unref(&pic);
                throw std::runtime_error("Unsupported pixel layout");
        }

        const int w = pic.p.w;
        const int h = pic.p.h;
        const uint32_t cw = (uint32_t)((w + ss_hor) >> ss_hor);
        const uint32_t ch = (uint32_t)((h + ss_ver) >> ss_ver);

        // DECODE DIRECTLY INTO outFrame's BUFFERS
        outFrame.textureYData.resize(static_cast<size_t>(w) * static_cast<size_t>(h));
        outFrame.textureUData.resize(static_cast<size_t>(cw) * static_cast<size_t>(ch));
        outFrame.textureVData.resize(static_cast<size_t>(cw) * static_cast<size_t>(ch));

        // copy planes respecting stride; destination is tightly packed (row bytes == width)
        const uint8_t* srcY = static_cast<const uint8_t*>(pic.data[0]);
        const int strideY = pic.stride[0];
        for (int row = 0; row < h; ++row) {
            std::memcpy(outFrame.textureYData.data() + (size_t)row * w, srcY + (size_t)row * strideY, (size_t)w);
        }
        outFrame.textureYStride = strideY;

        const uint8_t* srcU = static_cast<const uint8_t*>(pic.data[1]);
        const int strideU = pic.stride[1];
        for (uint32_t row = 0; row < ch; ++row) {
            std::memcpy(outFrame.textureUData.data() + (size_t)row * cw, srcU + (size_t)row * strideU, (size_t)cw);
        }
        outFrame.textureUStride = strideU;

        const uint8_t* srcV = static_cast<const uint8_t*>(pic.data[2]);
        const int strideV = pic.stride[1];
        for (uint32_t row = 0; row < ch; ++row) {
            std::memcpy(outFrame.textureVData.data() + (size_t)row * cw, srcV + (size_t)row * strideV, (size_t)cw);
        }
        outFrame.textureVStride = strideV;

        // fill output frame pointers and sizes
        outFrame.textureYWidth = static_cast<uint32_t>(w);
        outFrame.textureYHeight = static_cast<uint32_t>(h);

        outFrame.textureUWidth = cw;
        outFrame.textureUHeight = ch;

        outFrame.textureVWidth = cw;
        outFrame.textureVHeight = ch;

        outFrame.ts_us = pic.m.timestamp;

        dav1d_picture_unref(&pic);
        return true;
    } else if (r == -EAGAIN) {
        return false; // no picture available now
    } else {
        // other error (including EOS)
        if (r == DAV1D_ERR(EINVAL) || r == DAV1D_ERR(ENOMEM)) {
            throw std::runtime_error("dav1d_get_picture internal error");
        }
        return false;
    }
}

// HELPER FUNCTION to receive a frame from an FFmpeg decoder
static bool receive_ffmpeg_alpha_frame(AVCodecContext* ctx, AVFrame* frame, VideoFrame& outFrame) {
    int ret = avcodec_receive_frame(ctx, frame);
    if (ret == 0) {
        // Frame successfully received

        // Alpha frame (8-bit gray)
        outFrame.textureAlphaWidth = frame->width;
        outFrame.textureAlphaHeight = frame->height;
        outFrame.textureAlphaData.resize(frame->width * frame->height);
        for (int y = 0; y < frame->height; ++y) {
            memcpy(outFrame.textureAlphaData.data() + y * frame->width,
                   frame->data[0] + y * frame->linesize[0],
                   frame->width);
        }
        outFrame.textureAlphaStride = frame->linesize[0];
        av_frame_unref(frame);
        return true;
    }
    return false; // EAGAIN or other error
}

static bool receive_ffmpeg_depth_frame(AVCodecContext* ctx, SwsContext* swsCtx, AVFrame* frameBE, AVFrame* frameLE, VideoFrame& outFrame) {
    int ret = avcodec_receive_frame(ctx, frameBE);
    if (ret == 0) {
        // Frame successfully received

        // Depth frame (16-bit gray BE -> convert to LE)
        // Use SwsContext to convert from GRAY16BE to GRAY16LE
        sws_scale(swsCtx,
                  (const uint8_t *const *)frameBE->data, frameBE->linesize,
                  0, frameBE->height,
                  frameLE->data, frameBE->linesize);

        // Calculate required size and resize the vector
        // The size is (width * height) uint16_t elements.
        size_t num_pixels = (size_t)frameBE->width * frameBE->height;
        outFrame.textureDepthWidth = frameBE->width;
        outFrame.textureDepthHeight = frameBE->height;
        outFrame.textureDepthData.resize(num_pixels);

        // Define the source and destination pointers
        const uint8_t* src_ptr = frameLE->data[0];
        uint8_t* dest_ptr = (uint8_t*)outFrame.textureDepthData.data();

        // Define strides/sizes in bytes
        const int src_stride_bytes = frameLE->linesize[0];
        const int dst_stride_bytes = frameLE->width * sizeof(uint16_t); // Tightly packed destination
        const int row_bytes = dst_stride_bytes; // The actual pixel data bytes per row

        for (int y = 0; y < frameLE->height; ++y) {
            // Source: Start of the row in the AVFrame buffer.
            const uint8_t* src_row = src_ptr + y * src_stride_bytes;

            // Destination: Start of the row in the std::vector buffer.
            // Since the vector is tightly packed, the stride is simply 'row_bytes'.
            uint8_t* dest_row = dest_ptr + y * row_bytes;

            // Copy only the actual pixel data (row_bytes)
            memcpy(dest_row, src_row, row_bytes);

            // Error check (Optional but Recommended):
            if (row_bytes > src_stride_bytes) {
                // This case should not happen for AVFrame, but is a good check.
                // It indicates the destination is wider than the source stride.
                throw std::runtime_error("Destination row is wider than source linesize, potential data corruption.");
            }
        }
        av_frame_unref(frameBE);
        av_frame_unref(frameLE);
        return true;
    }
    return false; // EAGAIN or other error
}

// ---------- decode_next_frame: streaming (one frame) ----------
bool WebmInMemoryDemuxer::decode_next_frame(VideoFrame& outFrame) {
    if (!fmtCtx_ || !dav1dCtx_ || !alphaCodecCtx_ || !depthCodecCtx_) {
        throw std::runtime_error("Decoders not initialized");
    }

    bool has_color = false;
    bool has_alpha = false;
    bool has_depth = false;

    AVFrame* alpha_frame = av_frame_alloc();
    if (!alpha_frame) {
        av_frame_free(&alpha_frame);
        throw std::bad_alloc();
    }

    AVFrame* depth_frameBE = av_frame_alloc();
    if (!depth_frameBE) {
        av_frame_free(&depth_frameBE);
        throw std::bad_alloc();
    }

    AVFrame* depth_frameLE = av_frame_alloc();
    if (!depth_frameLE) {
        av_frame_free(&depth_frameLE);
        throw std::bad_alloc();
    }

    depth_frameLE->width = depthCodecCtx_->width;
    depth_frameLE->height = depthCodecCtx_->width;
    depth_frameLE->format = AV_PIX_FMT_GRAY16LE; // Set the target format
    if (av_frame_get_buffer(depth_frameLE, 0) < 0) {
        av_frame_free(&alpha_frame);
        av_frame_free(&depth_frameBE);
        av_frame_free(&depth_frameLE);
        throw std::runtime_error("Could not allocate depth_frameLE buffer");
    }

    // This loop continues until we have one frame from each stream
    while (!(has_color && has_alpha && has_depth)) {
        // Draining Loop
        // First, always try to drain any available frames from the decoders before reading a new packet.
        // This is the key to keeping the streams in sync.
        while (true) {
            bool received_any = false;
            if (!has_color) {
                if (get_next_dav1d_picture(outFrame)) {
                    has_color = true;
                    received_any = true;
                }
            }
            if (!has_alpha) {
                if (receive_ffmpeg_alpha_frame(alphaCodecCtx_, alpha_frame, outFrame)) {
                    has_alpha = true;
                    received_any = true;
                }
            }
            if (!has_depth) {
                if (receive_ffmpeg_depth_frame(depthCodecCtx_, swsCtx_, depth_frameBE, depth_frameLE, outFrame)) {
                    has_depth = true;
                    received_any = true;
                }
            }
            // If we got a complete set of frames, or if no decoder had a frame ready,
            // break out of the draining loop to read the next packet.
            if ((has_color && has_alpha && has_depth) || !received_any) {
                break;
            }
        }

        // If we have all three components, we're done.
        if (has_color && has_alpha && has_depth) break;

        // Otherwise, read a new packet from the container and feed it to the correct decoder.
        AVPacket pkt;
        if (!read_packet(&pkt)) {
            // End of stream, no more packets to read. Break the loop.
            break;
        }

        if (pkt.stream_index == colorStreamIndex_) {
            submit_packet_to_dav1d(&pkt);
        } else if (pkt.stream_index == alphaStreamIndex_) {
            avcodec_send_packet(alphaCodecCtx_, &pkt);
        } else if (pkt.stream_index == depthStreamIndex_) {
            avcodec_send_packet(depthCodecCtx_, &pkt);
        }

        av_packet_unref(&pkt);
    }

    av_frame_free(&alpha_frame);
    av_frame_free(&depth_frameBE);
    av_frame_free(&depth_frameLE);

    // Return true only if we successfully got all three components.
    return (has_color && has_alpha && has_depth);
}

// ---------- flush / reset ----------
void WebmInMemoryDemuxer::flush_decoders() {
    if (dav1dCtx_) dav1d_flush(dav1dCtx_);
    // FFmpeg codec contexts aren't used here (we use raw packets + dav1d directly),
    // but to be safe we can drop any internal buffers by seeking to current position.
    // avcodec_flush_buffers() would be used if we had an AVCodecContext open.
}
