// Dav1dPlayer.cpp
#include "Dav1dPlayer.h"
#include "Dav1d/tools/input/input.h"

#include <android/log.h>
#include "Core/Logging.h"

#include <vector>
#include <fstream>
#include <iostream>
#include <cstring>

Dav1dPlayer::Dav1dPlayer() : p{}, ctx_(nullptr), davData{}, in_ctx(nullptr) {}

Dav1dPlayer::~Dav1dPlayer() {
    free(rd_ctx);
    Shutdown();
}

static Dav1dPlayRenderContext *dp_rd_ctx_create()
{
    Dav1dPlayRenderContext *rd_ctx;

    // Alloc
    rd_ctx = static_cast<Dav1dPlayRenderContext*>(calloc(1, sizeof(Dav1dPlayRenderContext)));
    if (rd_ctx == NULL) {
        return NULL;
    }

    // Parse and validate arguments
    dav1d_default_settings(&rd_ctx->lib_settings);
    memset(&rd_ctx->settings, 0, sizeof(rd_ctx->settings));

    // Settings
    Dav1dPlaySettings *settings = &rd_ctx->settings;
    Dav1dSettings *lib_settings = &rd_ctx->lib_settings;

    // settings->inputfile = optarg; // input file\n"
    // fprintf(stderr, "%s\n", dav1d_version()); // print version
    // settings->untimed = true; // ignore PTS, render as fast as possible
    // settings->highquality = true; // enable high quality rendering
    // settings->zerocopy = true; // enable zero copy upload path
    // settings->gpugrain = true; // enable GPU grain synthesis
    // settings->fullscreen = true; // enable full screen mode
    // settings->renderer_name = optarg; // select renderer backend (default: auto)
    settings->renderer_name = NULL;
    lib_settings->n_threads = 0; // number of threads (default: 0)
    lib_settings->max_frame_delay = 0; // maximum frame delay, capped at $threads (default: 0)
                                       // set to 1 for low-latency decoding


/*
    // Init SDL2 library
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) < 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        goto fail;
    }

    // Register a custom event to notify our SDL main thread
    // about new frames
    rd_ctx->event_types = SDL_RegisterEvents(3);
    if (rd_ctx->event_types == UINT32_MAX) {
        fprintf(stderr, "Failure to create custom SDL event types!\n");
        goto fail;
    }

    rd_ctx->fifo = dp_fifo_create(5);
    if (rd_ctx->fifo == NULL) {
        fprintf(stderr, "Failed to create FIFO for output pictures!\n");
        goto fail;
    }

    rd_ctx->lock = SDL_CreateMutex();
    if (rd_ctx->lock == NULL) {
        fprintf(stderr, "SDL_CreateMutex failed: %s\n", SDL_GetError());
        goto fail;
    }

    // Select renderer
    renderer_info = dp_get_renderer(rd_ctx->settings.renderer_name);

    if (renderer_info == NULL) {
        printf("No suitable renderer matching %s found.\n",
               (rd_ctx->settings.renderer_name) ? rd_ctx->settings.renderer_name : "auto");
    } else {
        printf("Using %s renderer\n", renderer_info->name);
    }

    rd_ctx->rd_priv = (renderer_info) ? renderer_info->create_renderer(&rd_ctx->settings) : NULL;
    if (rd_ctx->rd_priv == NULL) {
        goto fail;
    }*/

    return rd_ctx;

    /*
    fail:
    if (rd_ctx->lock)
        SDL_DestroyMutex(rd_ctx->lock);
    if (rd_ctx->fifo)
        dp_fifo_destroy(rd_ctx->fifo);
    free(rd_ctx);
    SDL_Quit();
    return NULL;
     */
}

/*
bool Dav1dPlayer::Init() {
    Dav1dSettings s;
    dav1d_default_settings(&s);
    // s.cpu_flags = 0; // allow dav1d to autodetect the best flags
    // s.threads = 0 -> autodetect
    s.n_threads = 0;
    int res = dav1d_open(&ctx_, &s);
    if (res < 0) {
        ctx_ = nullptr;
        return false;
    }
    return true;
}
*/

/*bool Dav1dPlayer::Init() {
    Dav1dPlayRenderContext *rd_ctx = dp_rd_ctx_create();
    if (rd_ctx == NULL) {
        LOGE("Failed creating render context\n");
        return false;
    }


    if (rd_ctx->settings.zerocopy) {
        if (renderer_info->alloc_pic) {
            rd_ctx->lib_settings.allocator = (Dav1dPicAllocator) {
                    .cookie = rd_ctx->rd_priv,
                    .alloc_picture_callback = renderer_info->alloc_pic,
                    .release_picture_callback = renderer_info->release_pic,
            };
        } else {
            fprintf(stderr, "--zerocopy unsupported by selected renderer\n");
        }
    }

    if (rd_ctx->settings.gpugrain) {
        if (renderer_info->supports_gpu_grain) {
            rd_ctx->lib_settings.apply_grain = 0;
        } else {
            fprintf(stderr, "--gpugrain unsupported by selected renderer\n");
        }
    }


    return true;
}*/

void Dav1dPlayer::Shutdown() {
    if (ctx_) {
        dav1d_close(&ctx_);
        ctx_ = nullptr;
    }
}

bool Dav1dPlayer::DecodeFile(const std::string& filename, FrameCallback frameCb, int frame_load_cap) {
    if (!ctx_) return false;
    // Load file into memory
    std::ifstream f(filename, std::ios::binary | std::ios::ate);
    if (!f) return false;
    std::streamsize size = f.tellg();
    f.seekg(0, std::ios::beg);
    std::vector<uint8_t> fileData((size_t)size);
    if (!f.read(reinterpret_cast<char*>(fileData.data()), size)) return false;

    return Dav1dPlayer::Decode(fileData, frameCb);
}

int Dav1dPlayer::InitDecoder(std::vector<uint8_t> fetched) {
    int res = 0;
    unsigned total;
    unsigned timebase[2];
    unsigned fps[2];

    Dav1dPlayRenderContext *rd_ctx = dp_rd_ctx_create();
    if (rd_ctx == NULL) {
        LOGE("Failed creating render context\n");
        res = 1;
        return ShutdownDecoder(res);
    }

    if ((res = input_open(&in_ctx,
                          "ivf",
                          fetched,
                          fps,
                          &total,
                          timebase)) < 0)
    {
        LOGE("Failed to open demuxer");
        res = 1;
        return ShutdownDecoder(res);
    }

    rd_ctx->timebase = (double)timebase[1] / timebase[0];
    rd_ctx->spf = (double)fps[1] / fps[0];
    rd_ctx->total = total;

    if ((res = dav1d_open(&ctx_, &rd_ctx->lib_settings))) {
        LOGE("Failed opening dav1d decoder");
        res = 1;
        return ShutdownDecoder(res);
    }

    if ((res = input_read(in_ctx, &davData)) < 0) {
        LOGE("Failed demuxing input");
        res = 1;
        return ShutdownDecoder(res);
    }

    return res;
}

int Dav1dPlayer::ShutdownDecoder(int res) {
    //dp_rd_ctx_post_event(rd_ctx, rd_ctx->event_types + DAV1D_EVENT_DEC_QUIT);

    if (in_ctx)
        input_close(in_ctx);
    if (ctx_)
        dav1d_close(&ctx_);

    return (res != DAV1D_ERR(EAGAIN) && res < 0);
}

// Blocking decode of a file - reads file into memory then feeds dav1d
bool Dav1dPlayer::Decode(std::vector<uint8_t> fileData, FrameCallback frameCb, int frame_load_cap) {
    LOGI("Dav1dPlayer::Decode start");
    if (!ctx_) return false;

    LOGI("Dav1dPlayer::Decode 1");
    // Prepare Dav1dData and feed
    //Dav1dData davData;
    //dav1d_data_wrap(&davData, fileData.data(), fileData.size(), nullptr, nullptr);

    int send_res = dav1d_send_data(ctx_, &davData);
    LOGI("Dav1dPlayer::Decode send_res=%d", send_res);
    if (send_res < 0) {
        dav1d_data_unref(&davData);
        return false;
    }
    LOGI("Dav1dPlayer::Decode 2");

    // Pull frames until none left
    //int frames_loaded = 0;
    while (true) {
        //if (frame_load_cap != -1 && frames_loaded >= frame_load_cap) {
        //    break;
        //}
        LOGI("Dav1dPlayer::Decode 3");

        //Dav1dPicture pic;
        int get_res = dav1d_get_picture(ctx_, &p);
        LOGI("Dav1dPlayer::Decode %d", get_res);
        if (get_res < 0) {
            // DAV1D returns EAGAIN-like values; negative means no more frames for now
            break;
        }
        LOGI("Dav1dPlayer::Decode 4");

        // Prepare pointers for data and strides
        const uint8_t* data[3] = { nullptr, nullptr, nullptr};
        int stride[3] = { 0,0,0};

        // Dav1dPicture uses pic.data[] and pic.stride[]
        data[0] = static_cast<const uint8_t*>(p.data[0]);
        data[1] = static_cast<const uint8_t*>(p.data[1]);
        data[2] = static_cast<const uint8_t*>(p.data[2]);

        stride[0] = p.stride[0];
        stride[1] = p.stride[1];
        stride[2] = p.stride[2];

        int w = p.p.w;
        int h = p.p.h;

        LOGI("Dav1dPlayer::Decode 5");
        // Call user callback - it must copy or immediately upload before dav1d_picture_unref
        frameCb(data, stride, w, h);
        LOGI("Dav1dPlayer::Decode 6");

        // Free picture
        dav1d_picture_unref(&p);
        //frames_loaded += 1;
    }

    // free data wrapper
    dav1d_data_unref(&davData);
    LOGI("Dav1dPlayer::Decode end");
    return true;
}
