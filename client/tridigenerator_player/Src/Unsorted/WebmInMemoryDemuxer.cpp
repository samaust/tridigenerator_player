// WebmInMemoryDemuxer.cpp
#include "WebmInMemoryDemuxer.h"

#include <stdexcept>
#include <cstring>
#include <algorithm>
#include <iostream>

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/mem.h>
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

        // find AV1 video stream
        videoStreamIndex_ = -1;
        for (unsigned i = 0; i < fmtCtx_->nb_streams; ++i) {
            AVCodecParameters* cp = fmtCtx_->streams[i]->codecpar;
            if (cp && cp->codec_type == AVMEDIA_TYPE_VIDEO && cp->codec_id == AV_CODEC_ID_AV1) {
                videoStreamIndex_ = (int)i;
                break;
            }
        }
        if (videoStreamIndex_ < 0) throw std::runtime_error("no AV1 video stream found");

        AVStream* vst = fmtCtx_->streams[videoStreamIndex_];
        timeBase_ = vst->time_base;
        width_ = vst->codecpar ? vst->codecpar->width : 0;
        height_ = vst->codecpar ? vst->codecpar->height : 0;

        // init dav1d
        Dav1dSettings s;
        dav1d_default_settings(&s);
        s.n_threads = 0; // auto
        int r = dav1d_open(&dav1dCtx_, &s);
        if (r < 0) throw std::runtime_error("dav1d_open failed");

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
    AVStream* st = fmtCtx_->streams[videoStreamIndex_];
    AVRational us_tb = {1, 1000000};
    int64_t target_pts = av_rescale_q(0, us_tb, st->time_base);

    int r = avformat_seek_file(fmtCtx_, videoStreamIndex_, INT64_MIN, target_pts, INT64_MAX, AVSEEK_FLAG_BACKWARD);
    if (r < 0) {
        r = av_seek_frame(fmtCtx_, videoStreamIndex_, target_pts, AVSEEK_FLAG_BACKWARD);
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
    AVStream* st = fmtCtx_->streams[videoStreamIndex_];
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

        const uint8_t* srcU = static_cast<const uint8_t*>(pic.data[1]);
        const int strideU = pic.stride[1];
        for (uint32_t row = 0; row < ch; ++row) {
            std::memcpy(outFrame.textureUData.data() + (size_t)row * cw, srcU + (size_t)row * strideU, (size_t)cw);
        }

        const uint8_t* srcV = static_cast<const uint8_t*>(pic.data[2]);
        const int strideV = pic.stride[1];
        for (uint32_t row = 0; row < ch; ++row) {
            std::memcpy(outFrame.textureVData.data() + (size_t)row * cw, srcV + (size_t)row * strideV, (size_t)cw);
        }

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

// ---------- decode_next_frame: streaming (one frame) ----------
bool WebmInMemoryDemuxer::decode_next_frame(VideoFrame& outFrame) {
    if (!fmtCtx_ || !dav1dCtx_) throw std::runtime_error("decoder not initialized");

    // First: if dav1d already has a picture ready, return it immediately.
    if (get_next_dav1d_picture(outFrame)) return true;

    // Otherwise, read packets and feed dav1d until we get a picture or hit EOF.
    AVPacket pkt;
    // av_init_packet(&pkt); // deprecated

    while (true) {
        int ret = av_read_frame(fmtCtx_, &pkt);
        if (ret == AVERROR_EOF) {
            // drain remaining pictures: if we can get one, return it.
            if (get_next_dav1d_picture(outFrame)) {
                av_packet_unref(&pkt);
                return true;
            }
            // No more pictures -> signal EOS to caller by returning false
            av_packet_unref(&pkt);
            return false;
        }
        throw_if_ffmpeg_err(ret, "av_read_frame");

        if (pkt.stream_index != videoStreamIndex_) {
            av_packet_unref(&pkt);
            continue;
        }

        // submit packet to dav1d (this will create a pkt_ref and hand it to dav1d)
        submit_packet_to_dav1d(&pkt);
        // we can safely unref the local pkt now
        av_packet_unref(&pkt);

        // try to get a picture now
        if (get_next_dav1d_picture(outFrame)) {
            return true;
        }

        // else loop and read more packets
    }
}

// ---------- flush / reset ----------
void WebmInMemoryDemuxer::flush_decoders() {
    if (dav1dCtx_) dav1d_flush(dav1dCtx_);
    // FFmpeg codec contexts aren't used here (we use raw packets + dav1d directly),
    // but to be safe we can drop any internal buffers by seeking to current position.
    // avcodec_flush_buffers() would be used if we had an AVCodecContext open.
}
