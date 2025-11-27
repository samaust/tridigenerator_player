#pragma once
// Dav1dPlayer.h - minimal dav1d wrapper that decodes AV1 frames and calls a callback with raw planes.

#include <functional>
#include <string>

#include <android/log.h>
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "FrameLoader", __VA_ARGS__)

#include <dav1d/dav1d.h>

struct Dav1dContext;
struct Dav1dData;
struct Dav1dPicture;

/**
 * Settings structure
 * Hold all settings available for the player,
 * this is usually filled by parsing arguments
 * from the console.
 */
typedef struct {
    const char *inputfile;
    const char *renderer_name;
    int highquality;
    int untimed;
    int zerocopy;
    int gpugrain;
    int fullscreen;
} Dav1dPlaySettings;

/**
 * Render context structure
 * This structure contains informations necessary
 * to be shared between the decoder and the renderer
 * threads.
 */
typedef struct render_context
{
    Dav1dPlaySettings settings;
    Dav1dSettings lib_settings;

    // Renderer private data (passed to callbacks)
    void *rd_priv;

    // Lock to protect access to the context structure
    //SDL_mutex *lock;

    // Timestamp of last displayed frame (in timebase unit)
    int64_t last_ts;
    // Timestamp of last decoded frame (in timebase unit)
    int64_t current_ts;
    // Ticks when last frame was received
    uint32_t last_ticks;
    // PTS time base
    double timebase;
    // Seconds per frame
    double spf;
    // Number of frames
    uint32_t total;

    // Fifo
    //Dav1dPlayPtrFifo *fifo;

    // Custom SDL2 event types
    //uint32_t event_types;

    // User pause state
    uint8_t user_paused;
    // Internal pause state
    uint8_t paused;
    // Start of internal pause state
    uint32_t pause_start;
    // Duration of internal pause state
    uint32_t pause_time;

    // Seek accumulator
    int seek;

    // Indicates if termination of the decoder thread was requested
    uint8_t dec_should_terminate;
} Dav1dPlayRenderContext;


typedef struct DemuxerPriv DemuxerPriv;
typedef struct Demuxer {
    int priv_data_size;
    const char *name;
    int probe_sz;
    int (*probe)(const uint8_t *data);
    int (*open)(DemuxerPriv *ctx, const char *filename,
                unsigned fps[2], unsigned *num_frames, unsigned timebase[2]);
    int (*read)(DemuxerPriv *ctx, Dav1dData *data);
    int (*seek)(DemuxerPriv *ctx, uint64_t pts);
    void (*close)(DemuxerPriv *ctx);
} Demuxer;

struct DemuxerContext {
    DemuxerPriv *data;
    const Demuxer *impl;
    uint64_t priv_data[];
};

class Dav1dPlayer {
public:
    // FrameCallback: called on decode thread when a frame is decoded. The callback must copy/upload data
    // from the pointers before returning or ensure pointers remain valid until upload completes.
    typedef std::function<void(const uint8_t* data[4], const int stride[4], int width, int height)> FrameCallback;

    Dav1dPlayer();
    ~Dav1dPlayer();

    // Initialize dav1d with recommended options (threads=0 => autodetect).
    //bool Init();

    int InitDecoder(std::vector<uint8_t> fetched);
    int ShutdownDecoder(int res);

    // Decode the entire file at filename and call frameCb for each decoded frame.
    // This is a blocking call; adapt to your streaming requirements.
    bool DecodeFile(const std::string& filename, FrameCallback frameCb, int frame_load_cap = -1);
    bool Decode(std::vector<uint8_t> fileData, FrameCallback frameCb, int frame_load_cap = -1);

    // Shutdown and cleanup
    void Shutdown();

private:
    Dav1dPlayRenderContext *rd_ctx;
    Dav1dPicture p;
    Dav1dContext* ctx_;
    Dav1dData davData;
    DemuxerContext *in_ctx = NULL;


};
