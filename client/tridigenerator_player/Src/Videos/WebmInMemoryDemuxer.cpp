// WebmInMemoryDemuxer.cpp
#include "WebmInMemoryDemuxer.h"

#include <stdexcept>
#include <cstring>
#include <algorithm>
#include <chrono>
#include <iostream>

#if defined(__aarch64__)
#include <arm_neon.h>
#endif

#define LOG_TAG "WebmInMemoryDemuxer"
#include "../Core/Logging.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/mem.h>
#include <libavutil/pixdesc.h>
#include <libavutil/pixfmt.h>
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

static inline std::chrono::steady_clock::time_point timing_start(bool enabled) {
    return enabled
        ? std::chrono::steady_clock::now()
        : std::chrono::steady_clock::time_point{};
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
WebmInMemoryDemuxer::WebmInMemoryDemuxer(
        const std::vector<uint8_t>& blob,
        DecoderThreadConfig threadConfig)
        : blob_(blob),
          threadConfig_(threadConfig) {
    // members initialized in init()
}

WebmInMemoryDemuxer::~WebmInMemoryDemuxer() {
    // Cleanup
    clear_pending_packets();
    av_frame_free(&alphaFrame_);
    av_frame_free(&depthFrame_);
    av_frame_free(&audioFrame_);
    if (dav1dCtx_) {
        dav1d_close(&dav1dCtx_);
        dav1dCtx_ = nullptr;
    }
    avcodec_free_context(&alphaCodecCtx_);
    avcodec_free_context(&depthCodecCtx_);
    avcodec_free_context(&audioCodecCtx_);
    swr_free(&swrCtx_);

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
        audioStreamIndex_ = -1;

        for (unsigned i = 0; i < fmtCtx_->nb_streams; ++i) {
            AVStream* stream = fmtCtx_->streams[i];
            AVCodecParameters* cp = stream->codecpar;
            if (!cp) continue;

            if (cp->codec_type == AVMEDIA_TYPE_VIDEO) {
                if (cp->codec_id == AV_CODEC_ID_AV1 && colorStreamIndex_ == -1) {
                    colorStreamIndex_ = (int)i;
                    LOGI("Found AV1 color stream at index %d", i);
                } else if (cp->codec_id == AV_CODEC_ID_FFV1 && cp->format == AV_PIX_FMT_GRAY8 && alphaStreamIndex_ == -1) {
                    alphaStreamIndex_ = (int)i;
                    LOGI("Found FFV1 alpha stream (gray8) at index %d", i);
                } else if (
                    ((cp->codec_id == AV_CODEC_ID_PNG &&
                      cp->format == AV_PIX_FMT_GRAY16BE) ||
                     (cp->codec_id == AV_CODEC_ID_FFV1 &&
                      (cp->format == AV_PIX_FMT_GRAY16LE ||
                       cp->format == AV_PIX_FMT_GRAY16BE))) &&
                    depthStreamIndex_ == -1) {
                    depthStreamIndex_ = (int)i;
                    depthBigEndian_ = cp->format == AV_PIX_FMT_GRAY16BE;
                    LOGI(
                        "Found %s depth stream (%s) at index %d",
                        cp->codec_id == AV_CODEC_ID_PNG ? "PNG" : "FFV1",
                        depthBigEndian_ ? "gray16be" : "gray16le",
                        i);
                }
            } else if (cp->codec_type == AVMEDIA_TYPE_AUDIO && audioStreamIndex_ == -1) {
                audioStreamIndex_ = static_cast<int>(i);
                LOGI("Found optional audio stream at index %d", i);
            }
        }

        if (colorStreamIndex_ < 0 || alphaStreamIndex_ < 0 || depthStreamIndex_ < 0) {
            throw std::runtime_error("Failed to find all required streams (color, alpha, depth)");
        }

        // --- INITIALIZE DECODERS ---
#if defined(__ANDROID__)
        if (threadConfig_.preferHardwareColor) {
            mediaCodecColorDecoder_ =
                std::make_unique<AndroidMediaCodecColorDecoder>();
        }
        AVStream* colorStream = fmtCtx_->streams[colorStreamIndex_];
        std::string hardwareError;
        const AVRational guessedRate = av_guess_frame_rate(
            fmtCtx_, colorStream, nullptr);
        const double sourceFps =
            guessedRate.num > 0 && guessedRate.den > 0
            ? av_q2d(guessedRate)
            : 30.0;
        if (mediaCodecColorDecoder_ && mediaCodecColorDecoder_->Initialize(
                colorStream->codecpar,
                colorStream->time_base,
                sourceFps,
                threadConfig_.hardwareLowLatency,
                hardwareError)) {
            colorDecoderHardware_ = true;
            colorDecoderName_ = mediaCodecColorDecoder_->DecoderName();
            LOGI("Color decoder=MediaCodec hardware name=%s low-latency=%s",
                 colorDecoderName_.c_str(),
                 threadConfig_.hardwareLowLatency ? "on" : "off");
        } else {
            if (!threadConfig_.preferHardwareColor) {
                hardwareError = "hardware decoder disabled by benchmark mode";
            }
            colorDecoderFallbackReason_ = hardwareError;
            mediaCodecColorDecoder_.reset();
            LOGW("Hardware AV1 unavailable; fallback=dav1d reason=%s",
                 hardwareError.c_str());
        }
#endif
        if (!colorDecoderHardware_) {
            Dav1dSettings s;
            dav1d_default_settings(&s);
            s.n_threads = std::max(0, threadConfig_.colorThreads);
            s.max_frame_delay = std::max(1, threadConfig_.colorFrameDelay);
            s.apply_grain = 0;
            if (dav1d_open(&dav1dCtx_, &s) < 0) {
                throw std::runtime_error("dav1d_open failed");
            }
            colorDecoderName_ = "dav1d";
            LOGI(
                "Color fallback decoder=dav1d threads=%d frame-delay=%d film-grain=off",
                s.n_threads,
                s.max_frame_delay);
        }

        // FFmpeg decoder for alpha stream
        AVStream* alphaStream = fmtCtx_->streams[alphaStreamIndex_];
        const AVCodec* alphaCodec = avcodec_find_decoder(alphaStream->codecpar->codec_id);
        if (!alphaCodec) throw std::runtime_error("Could not find FFV1 decoder for alpha");
        alphaCodecCtx_ = avcodec_alloc_context3(alphaCodec);
        if (!alphaCodecCtx_) throw std::bad_alloc();
        throw_if_ffmpeg_err(avcodec_parameters_to_context(alphaCodecCtx_, alphaStream->codecpar), "alpha avcodec_parameters_to_context");
        alphaCodecCtx_->thread_count = std::max(1, threadConfig_.alphaThreads);
        alphaCodecCtx_->thread_type =
            (alphaCodec->capabilities & AV_CODEC_CAP_SLICE_THREADS)
            ? FF_THREAD_SLICE
            : FF_THREAD_FRAME;
        throw_if_ffmpeg_err(avcodec_open2(alphaCodecCtx_, alphaCodec, nullptr), "alpha avcodec_open2");
        LOGI(
            "Alpha decoder threads requested=%d active=%d type=%s",
            threadConfig_.alphaThreads,
            alphaCodecCtx_->thread_count,
            (alphaCodecCtx_->active_thread_type & FF_THREAD_SLICE)
                ? "slice"
                : (alphaCodecCtx_->active_thread_type & FF_THREAD_FRAME)
                    ? "frame"
                    : "none");

        // FFmpeg decoder for depth stream
        AVStream* depthStream = fmtCtx_->streams[depthStreamIndex_];
        const AVCodec* depthCodec = avcodec_find_decoder(depthStream->codecpar->codec_id);
        if (!depthCodec) throw std::runtime_error("Could not find PNG decoder for depth");
        depthCodecCtx_ = avcodec_alloc_context3(depthCodec);
        if (!depthCodecCtx_) throw std::bad_alloc();
        throw_if_ffmpeg_err(avcodec_parameters_to_context(depthCodecCtx_, depthStream->codecpar), "depth avcodec_parameters_to_context");
        depthCodecCtx_->thread_count = std::max(1, threadConfig_.depthThreads);
        depthCodecCtx_->thread_type =
            depthCodec->capabilities & AV_CODEC_CAP_FRAME_THREADS
            ? FF_THREAD_FRAME
            : FF_THREAD_SLICE;
        throw_if_ffmpeg_err(avcodec_open2(depthCodecCtx_, depthCodec, nullptr), "depth avcodec_open2");
        LOGI(
            "Depth decoder threads requested=%d active=%d type=%s",
            threadConfig_.depthThreads,
            depthCodecCtx_->thread_count,
            (depthCodecCtx_->active_thread_type & FF_THREAD_FRAME)
                ? "frame"
                : (depthCodecCtx_->active_thread_type & FF_THREAD_SLICE)
                    ? "slice"
                    : "none");

        alphaFrame_ = av_frame_alloc();
        depthFrame_ = av_frame_alloc();
        audioFrame_ = av_frame_alloc();
        if (!alphaFrame_ || !depthFrame_ || !audioFrame_) {
            throw std::bad_alloc();
        }

        if (audioStreamIndex_ >= 0) {
            AVStream* audioStream = fmtCtx_->streams[audioStreamIndex_];
            const AVCodec* audioCodec = avcodec_find_decoder(audioStream->codecpar->codec_id);
            if (!audioCodec) {
                LOGI("No decoder for optional audio stream; continuing silently");
                audioStreamIndex_ = -1;
            } else {
                audioCodecCtx_ = avcodec_alloc_context3(audioCodec);
                if (!audioCodecCtx_) throw std::bad_alloc();
                throw_if_ffmpeg_err(
                    avcodec_parameters_to_context(audioCodecCtx_, audioStream->codecpar),
                    "audio avcodec_parameters_to_context");
                throw_if_ffmpeg_err(
                    avcodec_open2(audioCodecCtx_, audioCodec, nullptr),
                    "audio avcodec_open2");
                AVChannelLayout outputLayout;
                av_channel_layout_default(&outputLayout, 2);
                const int resamplerResult = swr_alloc_set_opts2(
                    &swrCtx_,
                    &outputLayout, AV_SAMPLE_FMT_FLT, audioOutputSampleRate_,
                    &audioCodecCtx_->ch_layout, audioCodecCtx_->sample_fmt,
                    audioCodecCtx_->sample_rate, 0, nullptr);
                av_channel_layout_uninit(&outputLayout);
                if (resamplerResult < 0 || !swrCtx_ || swr_init(swrCtx_) < 0) {
                    LOGI("Could not initialize optional audio resampler; continuing silently");
                    swr_free(&swrCtx_);
                    avcodec_free_context(&audioCodecCtx_);
                    audioStreamIndex_ = -1;
                }
            }
        }

        // Set main timebase/width/height from the color stream
        AVStream* vst = fmtCtx_->streams[colorStreamIndex_];
        timeBase_ = vst->time_base;
        width_ = fmtCtx_->streams[colorStreamIndex_]->codecpar->width;
        height_ = fmtCtx_->streams[colorStreamIndex_]->codecpar->height;

        const AVCodecParameters* cp_color = fmtCtx_->streams[colorStreamIndex_]->codecpar;
        colorRangeKnown_ = (cp_color->color_range != AVCOL_RANGE_UNSPECIFIED);
        colorFullRange_ = (cp_color->color_range == AVCOL_RANGE_JPEG);

        // ready
        return true;
    } catch (const std::exception& ex) {
        if (error) *error = ex.what();
        // cleanup partial state
        if (dav1dCtx_) { dav1d_close(&dav1dCtx_); dav1dCtx_ = nullptr; }
        avcodec_free_context(&alphaCodecCtx_);
        avcodec_free_context(&depthCodecCtx_);
        avcodec_free_context(&audioCodecCtx_);
        clear_pending_packets();
        av_frame_free(&alphaFrame_);
        av_frame_free(&depthFrame_);
        av_frame_free(&audioFrame_);
        swr_free(&swrCtx_);
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
    clear_pending_packets();
    demuxEof_ = false;
    alphaDrainSent_ = false;
    depthDrainSent_ = false;
    audioDrainSent_ = false;
    nextFrameIndex_ = 0;
    pendingAudio_.clear();
    if (audioCodecCtx_) avcodec_flush_buffers(audioCodecCtx_);
    if (swrCtx_) {
        swr_close(swrCtx_);
        swr_init(swrCtx_);
    }
    return true;
}

std::vector<AudioPcmBlock> WebmInMemoryDemuxer::take_audio_blocks() {
    std::vector<AudioPcmBlock> result;
    result.swap(pendingAudio_);
    return result;
}

void WebmInMemoryDemuxer::drain_audio_frames() {
    if (!audioCodecCtx_ || !swrCtx_ || !audioFrame_) return;
    while (avcodec_receive_frame(audioCodecCtx_, audioFrame_) == 0) {
        const int outputFrames = static_cast<int>(av_rescale_rnd(
            swr_get_delay(swrCtx_, audioCodecCtx_->sample_rate) +
                audioFrame_->nb_samples,
            audioOutputSampleRate_, audioCodecCtx_->sample_rate, AV_ROUND_UP));
        AudioPcmBlock block;
        block.sampleRate = audioOutputSampleRate_;
        block.channels = 2;
        block.samples.resize(static_cast<size_t>(outputFrames) * 2);
        uint8_t* output[] = {
            reinterpret_cast<uint8_t*>(block.samples.data())
        };
        const int converted = swr_convert(
            swrCtx_, output, outputFrames,
            const_cast<const uint8_t**>(audioFrame_->extended_data),
            audioFrame_->nb_samples);
        if (converted > 0) {
            block.samples.resize(static_cast<size_t>(converted) * 2);
            int64_t pts = audioFrame_->best_effort_timestamp;
            if (pts == AV_NOPTS_VALUE) pts = audioFrame_->pts;
            const AVRational timeBase = fmtCtx_->streams[audioStreamIndex_]->time_base;
            block.timestampUs = pts == AV_NOPTS_VALUE
                ? 0
                : av_rescale_q(pts, timeBase, AVRational{1, 1000000});
            pendingAudio_.push_back(std::move(block));
        }
        av_frame_unref(audioFrame_);
    }
}

void WebmInMemoryDemuxer::decode_audio_packet(const AVPacket* pkt) {
    if (!audioCodecCtx_) return;
    int result = avcodec_send_packet(audioCodecCtx_, pkt);
    if (result == AVERROR(EAGAIN)) {
        drain_audio_frames();
        result = avcodec_send_packet(audioCodecCtx_, pkt);
    }
    if (result >= 0) {
        drain_audio_frames();
    } else {
        char error[128] = {};
        av_strerror(result, error, sizeof(error));
        LOGI("Optional audio packet was rejected: %s", error);
    }
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
    if (s == -EAGAIN) {
        dav1d_data_unref(&d);
        return false;
    }
    if (s < 0) {
        // dav1d_send_data failed -> we must release our pkt_ref because dav1d won't call the callback
        dav1d_data_unref(&d);
        throw std::runtime_error("dav1d_send_data failed");
    }

    // Success: pkt_ref will be freed by dav1d when it releases the data.
    return true;
}

// ---------- get next dav1d picture and populate outFrame ----------
bool WebmInMemoryDemuxer::get_next_dav1d_picture(
        VideoFrame& outFrame,
        DecodeInvocationTiming* timing) {
    Dav1dPicture pic;
    memset(&pic, 0, sizeof(pic));
    const auto codecStart = timing_start(timing != nullptr);
    int r = dav1d_get_picture(dav1dCtx_, &pic);
    if (timing) {
        timing->colorCodecMilliseconds +=
            std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - codecStart).count();
    }
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

        // Move the picture reference into the frame ring. The render thread
        // performs the sole row copy directly into its mapped upload buffer.
        auto* retained = new Dav1dPicture(pic);
        std::memset(&pic, 0, sizeof(pic));
        outFrame.colorPlaneOwner = std::shared_ptr<void>(
            retained,
            [](void* value) {
                auto* picture = static_cast<Dav1dPicture*>(value);
                dav1d_picture_unref(picture);
                delete picture;
            });
        outFrame.textureYData.clear();
        outFrame.textureUData.clear();
        outFrame.textureVData.clear();
        outFrame.colorPlaneViews = {{
            {static_cast<const uint8_t*>(retained->data[0]), static_cast<uint32_t>(w),
             static_cast<uint32_t>(h), static_cast<int>(retained->stride[0])},
            {static_cast<const uint8_t*>(retained->data[1]), cw, ch,
             static_cast<int>(retained->stride[1])},
            {static_cast<const uint8_t*>(retained->data[2]), cw, ch,
             static_cast<int>(retained->stride[1])},
        }};
        outFrame.textureYStride = static_cast<int>(retained->stride[0]);
        outFrame.textureUStride = static_cast<int>(retained->stride[1]);
        outFrame.textureVStride = static_cast<int>(retained->stride[1]);

        // fill output frame pointers and sizes
        outFrame.textureYWidth = static_cast<uint32_t>(w);
        outFrame.textureYHeight = static_cast<uint32_t>(h);

        outFrame.textureUWidth = cw;
        outFrame.textureUHeight = ch;

        outFrame.textureVWidth = cw;
        outFrame.textureVHeight = ch;

        outFrame.ts_us = retained->m.timestamp;
        if (colorRangeKnown_) {
            outFrame.yuvFullRange = colorFullRange_;
        } else {
            // Schema 4 requires limited range. Containers without range
            // signalling are therefore interpreted as limited, rather than
            // forcing a decoder-thread scan/copy.
            outFrame.yuvFullRange = false;
        }
        // colorCopyMilliseconds intentionally remains zero here: ownership
        // was transferred, not copied.
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

bool WebmInMemoryDemuxer::get_next_color_picture(
        VideoFrame& outFrame,
        DecodeInvocationTiming* timing) {
#if defined(__ANDROID__)
    if (mediaCodecColorDecoder_) {
        double waitMilliseconds = 0.0;
        if (!mediaCodecColorDecoder_->TryGetFrame(
                outFrame.hardwareColorImage,
                outFrame.ts_us,
                waitMilliseconds)) {
            return false;
        }
        outFrame.colorPlaneOwner.reset();
        outFrame.colorPlaneViews = {};
        outFrame.textureYData.clear();
        outFrame.textureUData.clear();
        outFrame.textureVData.clear();
        outFrame.textureYWidth = static_cast<uint32_t>(width_);
        outFrame.textureYHeight = static_cast<uint32_t>(height_);
        outFrame.textureUWidth = static_cast<uint32_t>((width_ + 1) / 2);
        outFrame.textureUHeight = static_cast<uint32_t>((height_ + 1) / 2);
        outFrame.textureVWidth = outFrame.textureUWidth;
        outFrame.textureVHeight = outFrame.textureUHeight;
        outFrame.yuvFullRange = false;
        if (timing) {
            timing->colorHardwareOutputWaitMilliseconds += waitMilliseconds;
        }
        return true;
    }
#endif
    outFrame.hardwareColorImage.reset();
    return get_next_dav1d_picture(outFrame, timing);
}

bool WebmInMemoryDemuxer::submit_color_packet(const AVPacket* packet) {
#if defined(__ANDROID__)
    if (mediaCodecColorDecoder_) {
        return mediaCodecColorDecoder_->QueuePacket(packet);
    }
#endif
    return submit_packet_to_dav1d(packet);
}

bool WebmInMemoryDemuxer::receive_alpha_frame(
        VideoFrame& outFrame,
        DecodeInvocationTiming* timing) {
    const auto codecStart = timing_start(timing != nullptr);
    int ret = avcodec_receive_frame(alphaCodecCtx_, alphaFrame_);
    if (timing) {
        timing->alphaCodecMilliseconds +=
            std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - codecStart).count();
    }
    if (ret == 0) {
        const auto copyStart = timing_start(timing != nullptr);
        outFrame.textureAlphaWidth = alphaFrame_->width;
        outFrame.textureAlphaHeight = alphaFrame_->height;
        outFrame.textureAlphaData.resize(
            static_cast<size_t>(alphaFrame_->width) * alphaFrame_->height);
        for (int y = 0; y < alphaFrame_->height; ++y) {
            memcpy(
                outFrame.textureAlphaData.data() +
                    static_cast<size_t>(y) * alphaFrame_->width,
                alphaFrame_->data[0] +
                    static_cast<size_t>(y) * alphaFrame_->linesize[0],
                alphaFrame_->width);
        }
        outFrame.textureAlphaStride = alphaFrame_->width;
        int64_t pts = alphaFrame_->best_effort_timestamp;
        if (pts == AV_NOPTS_VALUE) pts = alphaFrame_->pts;
        lastAlphaTimestampUs_ = pts_to_us(
            pts, fmtCtx_->streams[alphaStreamIndex_]->time_base);
        if (timing) {
            timing->alphaCopyMilliseconds +=
                std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - copyStart).count();
        }
        av_frame_unref(alphaFrame_);
        return true;
    }
    if (ret != AVERROR(EAGAIN) && ret != AVERROR_EOF) {
        throw_if_ffmpeg_err(ret, "alpha avcodec_receive_frame");
    }
    return false;
}

bool WebmInMemoryDemuxer::receive_depth_frame(
        VideoFrame& outFrame,
        DecodeInvocationTiming* timing) {
    const auto codecStart = timing_start(timing != nullptr);
    int ret = avcodec_receive_frame(depthCodecCtx_, depthFrame_);
    if (timing) {
        timing->depthCodecMilliseconds +=
            std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - codecStart).count();
    }
    if (ret == 0) {
        const auto copyStart = timing_start(timing != nullptr);
        const size_t num_pixels =
            static_cast<size_t>(depthFrame_->width) * depthFrame_->height;
        outFrame.textureDepthWidth = depthFrame_->width;
        outFrame.textureDepthHeight = depthFrame_->height;
        outFrame.textureDepthData.resize(num_pixels);
        for (int y = 0; y < depthFrame_->height; ++y) {
            const uint8_t* source =
                depthFrame_->data[0] +
                static_cast<size_t>(y) * depthFrame_->linesize[0];
            uint16_t* destination =
                outFrame.textureDepthData.data() +
                static_cast<size_t>(y) * depthFrame_->width;
            if (!depthBigEndian_) {
                memcpy(
                    destination,
                    source,
                    static_cast<size_t>(depthFrame_->width) * sizeof(uint16_t));
                continue;
            }
#if defined(__aarch64__)
            int x = 0;
            uint8_t* destinationBytes =
                reinterpret_cast<uint8_t*>(destination);
            for (; x + 8 <= depthFrame_->width; x += 8) {
                const uint8x16_t values = vld1q_u8(source + x * 2);
                vst1q_u8(destinationBytes + x * 2, vrev16q_u8(values));
            }
            for (; x < depthFrame_->width; ++x) {
                destination[x] =
                    static_cast<uint16_t>(
                        static_cast<uint16_t>(source[x * 2]) << 8) |
                    source[x * 2 + 1];
            }
#else
            for (int x = 0; x < depthFrame_->width; ++x) {
                destination[x] =
                    static_cast<uint16_t>(
                        static_cast<uint16_t>(source[x * 2]) << 8) |
                    source[x * 2 + 1];
            }
#endif
        }
        outFrame.textureDepthStride =
            depthFrame_->width * static_cast<int>(sizeof(uint16_t));
        int64_t pts = depthFrame_->best_effort_timestamp;
        if (pts == AV_NOPTS_VALUE) pts = depthFrame_->pts;
        lastDepthTimestampUs_ = pts_to_us(
            pts, fmtCtx_->streams[depthStreamIndex_]->time_base);
        if (timing) {
            timing->depthConvertCopyMilliseconds +=
                std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - copyStart).count();
        }
        av_frame_unref(depthFrame_);
        return true;
    }
    if (ret != AVERROR(EAGAIN) && ret != AVERROR_EOF) {
        throw_if_ffmpeg_err(ret, "depth avcodec_receive_frame");
    }
    return false;
}

bool WebmInMemoryDemuxer::send_or_queue_packet(
        AVCodecContext* context,
        const AVPacket* packet,
        std::deque<AVPacket*>& queue) {
    if (queue.empty()) {
        const int result = avcodec_send_packet(context, packet);
        if (result == 0) return true;
        if (result != AVERROR(EAGAIN)) {
            throw_if_ffmpeg_err(result, "avcodec_send_packet");
        }
    }
    if (queue.size() >= 16) {
        throw std::runtime_error("Decoder packet backlog exceeded 16 packets");
    }
    AVPacket* retained = av_packet_clone(packet);
    if (!retained) throw std::bad_alloc();
    queue.push_back(retained);
    return false;
}

bool WebmInMemoryDemuxer::submit_or_queue_color_packet(const AVPacket* packet) {
    if (pendingColorPackets_.empty() && submit_color_packet(packet)) {
        return true;
    }
    if (pendingColorPackets_.size() >= 16) {
        throw std::runtime_error("Color packet backlog exceeded 16 packets");
    }
    AVPacket* retained = av_packet_clone(packet);
    if (!retained) throw std::bad_alloc();
    pendingColorPackets_.push_back(retained);
    return false;
}

void WebmInMemoryDemuxer::clear_pending_packets() {
    const auto clear = [](std::deque<AVPacket*>& queue) {
        while (!queue.empty()) {
            AVPacket* packet = queue.front();
            queue.pop_front();
            av_packet_free(&packet);
        }
    };
    clear(pendingColorPackets_);
    clear(pendingAlphaPackets_);
    clear(pendingDepthPackets_);
}

// ---------- decode_next_frame: streaming (one frame) ----------
bool WebmInMemoryDemuxer::decode_next_frame(
        VideoFrame& outFrame,
        DecodeInvocationTiming* timing) {
    if (!fmtCtx_ || (!dav1dCtx_ && !colorDecoderHardware_) ||
        !alphaCodecCtx_ || !depthCodecCtx_) {
        throw std::runtime_error("Decoders not initialized");
    }
    if (timing) *timing = {};
    const auto totalStart = timing_start(timing != nullptr);

    bool has_color = false;
    bool has_alpha = false;
    bool has_depth = false;
    lastAlphaTimestampUs_ = AV_NOPTS_VALUE;
    lastDepthTimestampUs_ = AV_NOPTS_VALUE;

    // This loop continues until we have one frame from each stream
    while (!(has_color && has_alpha && has_depth)) {
        // Draining Loop
        // First, always try to drain any available frames from the decoders before reading a new packet.
        // This is the key to keeping the streams in sync.
        while (true) {
            bool received_any = false;
            if (!has_color) {
                if (get_next_color_picture(outFrame, timing)) {
                    has_color = true;
                    received_any = true;
                }
            }
            if (!has_alpha) {
                if (receive_alpha_frame(outFrame, timing)) {
                    has_alpha = true;
                    received_any = true;
                }
            }
            if (!has_depth) {
                if (receive_depth_frame(outFrame, timing)) {
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

        bool submittedPending = false;
        const auto submitPendingFfmpeg =
            [&](AVCodecContext* context, std::deque<AVPacket*>& queue) {
                if (queue.empty()) return false;
                const int result = avcodec_send_packet(context, queue.front());
                if (result == AVERROR(EAGAIN)) return false;
                throw_if_ffmpeg_err(result, "pending avcodec_send_packet");
                AVPacket* packet = queue.front();
                queue.pop_front();
                av_packet_free(&packet);
                return true;
            };
        if (!has_color && !pendingColorPackets_.empty() &&
            submit_color_packet(pendingColorPackets_.front())) {
            AVPacket* packet = pendingColorPackets_.front();
            pendingColorPackets_.pop_front();
            av_packet_free(&packet);
            submittedPending = true;
        }
        if (!has_alpha) {
            submittedPending =
                submitPendingFfmpeg(alphaCodecCtx_, pendingAlphaPackets_) ||
                submittedPending;
        }
        if (!has_depth) {
            submittedPending =
                submitPendingFfmpeg(depthCodecCtx_, pendingDepthPackets_) ||
                submittedPending;
        }
        if (submittedPending) continue;

        // Otherwise, read a new packet from the container and feed it to the correct decoder.
        AVPacket pkt;
        memset(&pkt, 0, sizeof(pkt));
        const auto demuxStart = timing_start(timing != nullptr);
        const bool read = !demuxEof_ && read_packet(&pkt);
        if (timing) {
            timing->demuxAudioMilliseconds +=
                std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - demuxStart).count();
        }
        if (!read) {
            demuxEof_ = true;
            bool sentDrain = false;
            const auto sendDrain = [&](AVCodecContext* context, bool& sent) {
                if (!context || sent) return false;
                const int result = avcodec_send_packet(context, nullptr);
                if (result == AVERROR(EAGAIN)) return true;
                if (result != AVERROR_EOF) {
                    throw_if_ffmpeg_err(result, "decoder drain");
                }
                sent = true;
                return true;
            };
            sentDrain = sendDrain(alphaCodecCtx_, alphaDrainSent_) || sentDrain;
            sentDrain = sendDrain(depthCodecCtx_, depthDrainSent_) || sentDrain;
            sentDrain = sendDrain(audioCodecCtx_, audioDrainSent_) || sentDrain;
            if (audioCodecCtx_) drain_audio_frames();
            if (sentDrain) continue;
            break;
        }

        if (pkt.stream_index == colorStreamIndex_) {
            const auto start = timing_start(timing != nullptr);
            if (has_color) {
                if (pendingColorPackets_.size() >= 16) {
                    av_packet_unref(&pkt);
                    throw std::runtime_error(
                        "Color packet backlog exceeded 16 packets");
                }
                AVPacket* retained = av_packet_clone(&pkt);
                if (!retained) {
                    av_packet_unref(&pkt);
                    throw std::bad_alloc();
                }
                pendingColorPackets_.push_back(retained);
            } else {
                submit_or_queue_color_packet(&pkt);
            }
            if (timing) {
                timing->colorCodecMilliseconds +=
                    std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - start).count();
            }
        } else if (pkt.stream_index == alphaStreamIndex_) {
            const auto start = timing_start(timing != nullptr);
            send_or_queue_packet(
                alphaCodecCtx_, &pkt, pendingAlphaPackets_);
            if (timing) {
                timing->alphaCodecMilliseconds +=
                    std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - start).count();
            }
        } else if (pkt.stream_index == depthStreamIndex_) {
            const auto start = timing_start(timing != nullptr);
            send_or_queue_packet(
                depthCodecCtx_, &pkt, pendingDepthPackets_);
            if (timing) {
                timing->depthCodecMilliseconds +=
                    std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - start).count();
            }
        } else if (pkt.stream_index == audioStreamIndex_) {
            const auto start = timing_start(timing != nullptr);
            decode_audio_packet(&pkt);
            if (timing) {
                timing->demuxAudioMilliseconds +=
                    std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - start).count();
            }
        }

        av_packet_unref(&pkt);
    }

    // Return true only if we successfully got all three components.
    const bool complete = has_color && has_alpha && has_depth;
    if (complete) {
        const auto timestampsMatch = [](int64_t a, int64_t b) {
            return a == AV_NOPTS_VALUE || b == AV_NOPTS_VALUE ||
                std::llabs(a - b) <= 1;
        };
        if (!timestampsMatch(outFrame.ts_us, lastAlphaTimestampUs_) ||
            !timestampsMatch(outFrame.ts_us, lastDepthTimestampUs_)) {
            throw std::runtime_error(
                "Decoded timestamps are not synchronized: color=" +
                std::to_string(outFrame.ts_us) + "us alpha=" +
                std::to_string(lastAlphaTimestampUs_) + "us depth=" +
                std::to_string(lastDepthTimestampUs_) + "us");
        }
        outFrame.frameIndex = nextFrameIndex_++;
    }
    if (timing) {
        timing->totalMilliseconds =
            std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - totalStart).count();
    }
    return complete;
}

// ---------- flush / reset ----------
void WebmInMemoryDemuxer::flush_decoders() {
    if (dav1dCtx_) dav1d_flush(dav1dCtx_);
#if defined(__ANDROID__)
    if (mediaCodecColorDecoder_) mediaCodecColorDecoder_->Flush();
#endif
    if (alphaCodecCtx_) avcodec_flush_buffers(alphaCodecCtx_);
    if (depthCodecCtx_) avcodec_flush_buffers(depthCodecCtx_);
    if (audioCodecCtx_) avcodec_flush_buffers(audioCodecCtx_);
    // FFmpeg codec contexts aren't used here (we use raw packets + dav1d directly),
    // but to be safe we can drop any internal buffers by seeking to current position.
    // avcodec_flush_buffers() would be used if we had an AVCodecContext open.
}
