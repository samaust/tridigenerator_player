/// \class FrameLoaderSystem
/// \brief Manages asynchronous video frame loading and playback for WebM format videos.
///
/// This system handles loading video manifests from a remote HTTP server, downloading and
/// demuxing WebM video files, maintaining a ring buffer of decoded frames for lock-free
/// producer-consumer synchronization, and coordinating frame presentation timing based on
/// configurable FPS. Uses a background thread for video decoding to avoid blocking the main
/// thread, with atomic operations for safe frame sharing between decoder and consumer.

#include <android/log.h>

#include <chrono>
#include <cstring>
#include <stdexcept>
#include <inttypes.h>

#include <curl/curl.h>
#include <json/json.h>

#include "Videos/WebmInMemoryDemuxer.h"

#define LOG_TAG "FrameLoaderSystem"
#include "../Core/Logging.h"

#include "FrameLoaderSystem.h"

#include "../Components/FrameLoaderComponent.h"
#include "../States/FrameLoaderState.h"

static constexpr int RING_SIZE = 8;

// ---------- writeBinary / writeString helpers for curl ----------
/**
 * @brief Curl write callback that appends incoming bytes into a std::string.
 *
 * This function is used as `CURLOPT_WRITEFUNCTION` when performing HTTP
 * requests that return textual payloads (e.g. JSON manifests). It appends
 * the received buffer to the provided `std::string` userdata and returns the
 * number of bytes processed.
 *
 * @param ptr Pointer to received data.
 * @param size Size of each member (as provided by curl).
 * @param nmemb Number of members.
 * @param userdata Pointer to a `std::string` that will receive the data.
 * @return Number of bytes written to the string.
 */
size_t FrameLoaderSystem::writeString(void* ptr, size_t size, size_t nmemb, void* userdata) {
    std::string* str = reinterpret_cast<std::string*>(userdata);
    size_t total = size * nmemb;
    str->append(reinterpret_cast<char*>(ptr), total);
    return total;
}
/**
 * @brief Curl write callback that appends incoming bytes into a byte vector.
 *
 * Used for binary downloads (video blobs). Appends the received data to the
 * provided `std::vector<uint8_t>` passed via userdata and returns the number
 * of bytes appended.
 *
 * @param ptr Pointer to received data.
 * @param size Size of each member (as provided by curl).
 * @param nmemb Number of members.
 * @param userdata Pointer to a `std::vector<uint8_t>` that will receive the data.
 * @return Number of bytes written to the vector.
 */
size_t FrameLoaderSystem::writeBinary(void* ptr, size_t size, size_t nmemb, void* userdata) {
    std::vector<uint8_t>* data = reinterpret_cast<std::vector<uint8_t>*>(userdata);
    size_t total = size * nmemb;
    uint8_t* start = reinterpret_cast<uint8_t*>(ptr);
    data->insert(data->end(), start, start + total);
    return total;
}

/**
 * @brief Initialize the FrameLoader system for entities that contain
 *        FrameLoaderComponent and FrameLoaderState.
 *
 * Prepares the ring buffer and frame pool, loads the remote manifest and
 * starts the background writer thread responsible for decoding frames.
 *
 * @param ecs Reference to the entity manager used to iterate entities.
 * @return true on successful initialization.
 */
bool FrameLoaderSystem::Init(EntityManager& ecs) {
    ecs.ForEachMulti<FrameLoaderComponent, FrameLoaderState>(
        [&](EntityID e,
            FrameLoaderComponent& flC,
            FrameLoaderState& flS) {
        flC.baseUrl = std::string("http://192.168.111.250:8080");
        flS.framePool.resize(RING_SIZE);
        flS.ring.resize(RING_SIZE);

        LoadManifest(flC, flS);
        StartBackgroundWriter(flC, flS);
    });
    return true;
}

/**
 * @brief Shutdown the FrameLoader system and stop background work.
 *
 * Signals the background writer to stop and joins the writer thread for
 * each entity that contains FrameLoader components.
 *
 * @param ecs Reference to the entity manager used to iterate entities.
 */
void FrameLoaderSystem::Shutdown(EntityManager& ecs) {
    ecs.ForEachMulti<FrameLoaderComponent, FrameLoaderState>(
            [&](EntityID e,
                FrameLoaderComponent& flC,
                FrameLoaderState& flS) {
        StopBackgroundWriter(flC, flS);
    });
}

/**
 * @brief Per-frame update for the frame loader system.
 *
 * Called from the main loop; this will attempt to swap in the next ready
 * decoded frame based on configured FPS and notify downstream consumers.
 *
 * @param ecs Reference to the entity manager used to iterate entities.
 * @param nowSeconds Current time in seconds (steady clock) used for scheduling.
 */
void FrameLoaderSystem::Update(EntityManager& ecs, double nowSeconds) {
    ecs.ForEachMulti<FrameLoaderComponent, FrameLoaderState>(
            [&](EntityID e,
                     FrameLoaderComponent &flC,
                     FrameLoaderState &flS) {
        // Save framePtr to component state for use in rendering
        // based on fps and newReadTime
        SwapNextFrame(nowSeconds,flC, flS);
    });
}

/**
 * @brief Load the remote JSON manifest for the currently configured base URL.
 *
 * Performs an HTTP GET to `baseUrl/manifest/frames.json`, parses JSON fields
 * such as file, width, height, fps and depth_scale_factor and stores them in
 * the component. Also resets internal read/write indices and nextReadTime.
 *
 * @param flC FrameLoader component holding configuration and results.
 * @param flS FrameLoader state that will be (re)initialized.
 * @return true on success, false on failure (HTTP or parse error).
 */
bool FrameLoaderSystem::LoadManifest(FrameLoaderComponent& flC,
                                     FrameLoaderState& flS) {
    std::string url = flC.baseUrl + "/manifest/frames.json";
    std::string jsonStr;
    // reuse HttpGetBinary? No, this one uses string body
    CURL* curl = curl_easy_init();
    if (!curl) {
        LOGE("curl init failed");
        return false;
    }
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeString);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &jsonStr);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    if (res != CURLE_OK) {
        LOGE("Failed GET manifest %s", url.c_str());
        return false;
    }

    Json::Value root;
    Json::Reader reader;
    if (!reader.parse(jsonStr, root)) {
        LOGE("Failed to parse manifest JSON");
        return false;
    }

    if (root.isMember("file") && root["file"].isString()) {
        flC.file = root["file"].asString();
    }
    if (root.isMember("width") && root["width"].isIntegral()) {
        flC.width = root["width"].asInt();
    }
    if (root.isMember("height") && root["height"].isIntegral()) {
        flC.height = root["height"].asInt();
    }
    if (root.isMember("fps") && root["fps"].isIntegral()) {
        flC.fps = root["fps"].asInt();
    }
    if (root.isMember("depth_scale_factor") && root["depth_scale_factor"].isNumeric()) {
        flC.depthScaleFactor = root["depth_scale_factor"].asFloat();
    }

    // reset indices
    flS.writeIdx.store(0);
    flS.readIdx.store(0);

    // initialize nextReadTime to now (consumer will read immediately)
    {
        std::lock_guard<std::mutex> lk(flS.timingMutex);
        using clock = std::chrono::steady_clock;
        flS.nextReadTime = std::chrono::duration<double>(clock::now().time_since_epoch()).count();
    }

    LOGI("Loaded manifest: file=%s width=%d height=%d fps=%d",
         flC.file.c_str(), flC.width, flC.height, flC.fps);

    return true;
}

/**
 * @brief Start the background writer thread which fetches and decodes frames.
 *
 * If a writer thread is not already running for the component, this will
 * mark it running and spawn `WriterLoop` in a new std::thread.
 *
 * @param flC FrameLoader component controlling the writer state.
 * @param flS FrameLoader state which stores thread handles and signaling primitives.
 */
void FrameLoaderSystem::StartBackgroundWriter(FrameLoaderComponent& flC,
                                              FrameLoaderState& flS) {
    bool expected = false;
    if (!flC.writerRunning.compare_exchange_strong(expected, true)) return; // already running
    flS.writerThread = std::thread(&FrameLoaderSystem::WriterLoop, this, std::ref(flC), std::ref(flS));
}

/**
 * @brief Stop the background writer thread and wait for it to exit.
 *
 * Signals the writer to stop and joins the thread if joinable.
 *
 * @param flC FrameLoader component controlling the writer state.
 * @param flS FrameLoader state which stores thread handles and signaling primitives.
 */
void FrameLoaderSystem::StopBackgroundWriter(FrameLoaderComponent& flC,
                                             FrameLoaderState& flS) {
    bool expected = true;
    if (!flC.writerRunning.compare_exchange_strong(expected, false)) {
        // not running
        return;
    }
    flS.writerCv.notify_all();
    if (flS.writerThread.joinable()) flS.writerThread.join();
}

/**
 * @brief Background writer loop that downloads and decodes video frames.
 *
 * Runs in a separate thread. Downloads the WebM blob from the server,
 * initializes an in-memory demuxer/decoder and continuously decodes frames
 * into the ring buffer while respecting the configured looping and free
 * slots to avoid overflow.
 *
 * @param flC FrameLoader component with configuration such as baseUrl and file.
 * @param flS FrameLoader state which contains the ring buffer and synchronization primitives.
 */
void FrameLoaderSystem::WriterLoop(FrameLoaderComponent& flC, FrameLoaderState& flS) {
    LOGI("Writer thread started");
    const int TARGET_FILL = RING_SIZE / 2; // keep half-filled

    // Download video
    std::vector<uint8_t> fetched = LoadVideoFromUrl(flC.baseUrl, flC.file);

    // Create and init demuxer
    WebmInMemoryDemuxer demuxer(fetched);
    std::string error;
    if (!demuxer.init(&error)) {
        LOGE("Failed to init demuxer: %s", error.c_str());
        flC.writerRunning.store(false, std::memory_order_release);
        return;
    }
    LOGI("Demuxer initialized: video %dx%d", demuxer.width(), demuxer.height());

    while (flC.writerRunning.load(std::memory_order_relaxed)) {
        // Wait until there is at least one free slot (or stop)
        {
            std::unique_lock<std::mutex> lk(flS.writerMutex);
            flS.writerCv.wait_for(lk, std::chrono::milliseconds(10), [&]() {
                return !flC.writerRunning.load(std::memory_order_relaxed) || ComputeFreeSlots(flS) > 0;
            });
        }
        if (!flC.writerRunning.load(std::memory_order_relaxed)) break;

        // Re-check free slots
        int freeSlots = ComputeFreeSlots(flS);
        if (freeSlots == 0) {
            // nothing to do
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }

        // Optionally limit how many frames we'll prefetch per loop to avoid hogging network
        int toFetch = std::min(freeSlots, TARGET_FILL);

        for (int i = 0; i < toFetch && flC.writerRunning.load(std::memory_order_relaxed); ++i) {
            // Before fetching, check again to avoid racing with reader
            freeSlots = ComputeFreeSlots(flS);
            if (freeSlots == 0) break;

            int slot = flS.writeIdx.load(std::memory_order_acquire);
            FrameSlot& s = flS.ring[slot];

            // Sanity: only write to a slot that is not ready
            if (!s.ready.load(std::memory_order_acquire)) {
                // Get a pointer to the frame we will write into.
                s.frame = &flS.framePool[slot];

                // Decode the next frame, passing our target buffer.
                if (!demuxer.decode_next_frame(*(s.frame))) {
                    // End of stream

                    // Check looping
                    if (!flC.looping.load(std::memory_order_acquire)) {
                        // stop writer
                        flC.writerRunning.store(false, std::memory_order_release);
                        break;
                    }

                    // seek to start
                    if (!demuxer.seek_to_start()) {
                        LOGI("seek_to_start() failed.");
                        // stop writer
                        flC.writerRunning.store(false, std::memory_order_release);
                        break;
                    }
                    continue;
                }

                // LOGI("Writer decoded frame ts=%" PRId64 "\n", frame.ts_us);

                // Mark ready (we don't move data anymore)
                s.ready.store(true, std::memory_order_release);

                // Advance write index
                flS.writeIdx.store((slot + 1) % RING_SIZE, std::memory_order_release);
            } else {
                // Slot unexpectedly still ready; avoid overwriting it
                // Sleep and retry
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }

        // If still running, small sleep to avoid busy spin
        //std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } // while
    LOGI("Writer thread exiting");
}

/**
 * @brief Load the video file blob from the remote server.
 *
 * Constructs the video URL from `baseUrl` and `file`, performs an HTTP GET
 * (binary) and returns the downloaded byte vector. On failure an empty
 * vector is returned.
 *
 * @param baseUrl Base URL (e.g. http://host:port).
 * @param file Filename as reported by the manifest.
 * @return Byte vector containing the video file, or empty on failure.
 */
std::vector<uint8_t> FrameLoaderSystem::LoadVideoFromUrl(std::string& baseUrl, std::string file) {
    std::vector<uint8_t> blob;

    std::string url = baseUrl + "/frames/" + file.c_str();
    LOGI("Loading video from %s", url.c_str());
    if (!HttpGetBinary(url, blob)) {
        LOGE("HttpGetBinary failed for %s", url.c_str());
        return blob;
    }
    return blob;
}

/**
 * @brief Perform an HTTP GET for binary data and store it in `out`.
 *
 * Uses libcurl to download the URL into the provided output vector. Reserves
 * capacity if the server reports Content-Length. Returns true on success.
 *
 * @param url The resource URL to fetch.
 * @param out Output byte vector that will receive the downloaded data.
 * @return true if the download succeeded, false otherwise.
 */
bool FrameLoaderSystem::HttpGetBinary(const std::string& url, std::vector<uint8_t>& out) {
    CURL* curl = curl_easy_init();
    if (!curl) return false;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeBinary);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &out);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_BUFFERSIZE, 1024 * 1024);
    curl_easy_setopt(curl, CURLOPT_TCP_NODELAY, 1L);  // disable Nagle
    curl_easy_setopt(curl, CURLOPT_TCP_FASTOPEN, 1L); // if supported

    curl_off_t contentLength = -1;
    if (curl_easy_getinfo(curl, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &contentLength) == CURLE_OK) {
        if (contentLength > 0) {
            out.reserve(static_cast<size_t>(contentLength));
        }
    }

    // Optionally set timeouts here
    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    return (res == CURLE_OK);
}

/**
 * @brief Compute the number of free slots available in the ring buffer.
 *
 * Uses the current write and read indices to determine how many frames can
 * still be written without overwriting unread frames. Always leaves one
 * slot empty to disambiguate full vs empty buffer.
 *
 * @param flS FrameLoader state containing `writeIdx` and `readIdx`.
 * @return The number of free slots available for writing.
 */
int FrameLoaderSystem::ComputeFreeSlots(FrameLoaderState& flS) const {
    int w = flS.writeIdx.load(std::memory_order_acquire);
    int r = flS.readIdx.load(std::memory_order_acquire);

    // This is the classic algorithm for calculating used space in a circular buffer.
    int usedSlots = (w - r + RING_SIZE) % RING_SIZE;

    // The number of free slots is the total size minus what's used.
    // We must always leave at least one slot empty to distinguish a full buffer
    // from an empty one (where w == r).
    int freeSlots = RING_SIZE - usedSlots - 1;

    return freeSlots;
}

/**
 * @brief Update the target playback frames-per-second.
 *
 * Sets the new FPS value in the component and adjusts the `nextReadTime`
 * to avoid large scheduling jumps by resetting the timer to now.
 *
 * @param newFps New frames-per-second target.
 * @param flC FrameLoader component to update.
 * @param flS FrameLoader state whose timing will be adjusted.
 */
void FrameLoaderSystem::SetFPS(int newFps, FrameLoaderComponent& flC, FrameLoaderState& flS) {
    flC.fps = newFps;
    // adjust nextReadTime to avoid huge skips
    std::lock_guard<std::mutex> lk(flS.timingMutex);
    using clock = std::chrono::steady_clock;
    flS.nextReadTime = std::chrono::duration<double>(clock::now().time_since_epoch()).count();
}

/**
 * @brief Attempt to swap the next ready frame into `framePtr` for consumption.
 *
 * This function checks timing against `nextReadTime` (based on FPS). If it's
 * time to advance and the next read slot is marked ready by the writer, the
 * slot is consumed atomically, `framePtr`/`frameReady` are updated and the
 * read index advances. Notifies the writer that a slot was freed.
 *
 * @param nowSeconds Current time in seconds (steady clock) used for scheduling.
 * @param flC FrameLoader component containing playback settings.
 * @param flS FrameLoader state containing the ring buffer and indices.
 * @return true if a new frame was consumed and is available via `flS.framePtr`.
 */
bool FrameLoaderSystem::SwapNextFrame(
        double nowSeconds,
        FrameLoaderComponent& flC,
        FrameLoaderState& flS) {
    // 1. Perform timing checks to see if we should advance to a new frame.
    int localFps = flC.fps;
    if (localFps <= 0) localFps = 1;
    double period = 1.0 / double(localFps);

    {
        std::lock_guard<std::mutex> lk(flS.timingMutex);
        if (nowSeconds < flS.nextReadTime) {
            return false; // Not time to show the next frame yet.
        }
        // It's time. Schedule the next frame presentation time.
        flS.nextReadTime += period;
        // Prevent drift if we're lagging: ensure nextReadTime > nowSeconds
        if (flS.nextReadTime <= nowSeconds) {
            flS.nextReadTime = nowSeconds + period;
        }
    }

    // Check if the next slot to be read has data ready from the writer.
    int currentReadSlot = flS.readIdx.load(std::memory_order_acquire);
    if (!flS.ring[currentReadSlot].ready.load(std::memory_order_acquire)) {
        // The writer hasn't produced a new frame for us yet.
        return false;
    }

    //LOGI("new frame ready");
    // The slot is ready. Give the caller a pointer to this frame's data.
    flS.framePtr = &flS.ring[currentReadSlot].frame;
    flS.frameReady.store(true, std::memory_order_release); // Signal that a new frame is ready

    // IMPORTANT: Mark the slot as "no longer ready for reading by the main thread"
    //            and advance the read pointer. This is the atomic "consumption" step.
    flS.ring[currentReadSlot].ready.store(false, std::memory_order_release);
    flS.readIdx.store((currentReadSlot + 1) % RING_SIZE, std::memory_order_release);

    // Notify the writer thread that a slot has become free.
    flS.writerCv.notify_one();

    // Return true to signal that a new frame has been "consumed" and *framePtr is valid.
    return true;
}