/// \class FrameLoaderSystem
/// \brief Manages asynchronous video frame loading and playback for WebM format videos.
///
/// This system handles loading video manifests from a remote HTTP server, downloading and
/// demuxing WebM video files, maintaining a ring buffer of decoded frames for lock-free
/// producer-consumer synchronization, and coordinating frame presentation timing based on
/// configurable FPS. Uses a background thread for video decoding to avoid blocking the main
/// thread, with atomic operations for safe frame sharing between decoder and consumer.

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <inttypes.h>

#if defined(ANDROID)
#include <curl/curl.h>
#endif

#include "Videos/WebmInMemoryDemuxer.h"

#define LOG_TAG "FrameLoaderSystem"
#include "../Core/Logging.h"

#include "FrameLoaderSystem.h"

#include "../Components/FrameLoaderComponent.h"
#include "../States/FrameLoaderState.h"
#include "../Data/VipeDataset.h"
#include "../Data/PlaybackControl.h"

static constexpr int RING_SIZE = 8;
static constexpr size_t MAX_AUDIO_SAMPLES = 48000 * 2 * 2;

static void ResetAudioState(FrameLoaderState& state) {
    std::lock_guard<std::mutex> lock(state.audioMutex);
    state.audioQueue.clear();
    state.audioSampleOffset = 0;
    state.audioAvailable.store(false, std::memory_order_release);
    state.audioStarted.store(false, std::memory_order_release);
    state.audioPlayedFrames.store(0, std::memory_order_release);
    state.audioEpochUs.store(0, std::memory_order_release);
}

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
#if defined(ANDROID)
        flC.baseUrl = std::string("http://192.168.111.250:8080");
#else
        flC.dataDirectory = "vipe_encoded";
#endif
        flS.framePool.resize(RING_SIZE);
        flS.ring.resize(RING_SIZE);

        if (LoadCatalog(flC) && !flC.catalog.datasets.empty()) {
            SelectDataset(flC.catalog.datasets.front().id, flC, flS);
        }
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
        if (ShouldConsumePlaybackFrame(flC.paused)) {
            SwapNextFrame(nowSeconds,flC, flS);
        }
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
    std::vector<uint8_t> bytes;
    if (!ReadResource(flC.manifestLocation, bytes)) {
        flC.errorMessage = "Failed to load manifest: " + flC.manifestLocation;
        LOGE("%s", flC.errorMessage.c_str());
        return false;
    }
    const std::string jsonText(bytes.begin(), bytes.end());
    std::string parseError;
    if (!ParseVipeDataset(jsonText, flC.dataset, parseError)) {
        flC.errorMessage = "Invalid ViPE manifest: " + parseError;
        LOGE("%s", flC.errorMessage.c_str());
        return false;
    }
    flC.file = flC.dataset.videoFile;
    flC.width = flC.dataset.width;
    flC.height = flC.dataset.height;
    flC.fps = static_cast<double>(flC.dataset.frameRateNumerator) /
        static_cast<double>(flC.dataset.frameRateDenominator);
    flC.depthScaleFactor = flC.dataset.depthUnitsPerMetre;
#if defined(ANDROID)
    const std::string::size_type slash = flC.manifestLocation.find_last_of('/');
    flC.videoLocation = (slash == std::string::npos)
        ? flC.baseUrl + "/" + flC.file
        : flC.manifestLocation.substr(0, slash + 1) + flC.file;
#else
    flC.videoLocation =
        (std::filesystem::path(flC.manifestLocation).parent_path() / flC.file).string();
#endif
    flC.errorMessage.clear();

    // reset indices
    flS.writeIdx.store(0);
    flS.readIdx.store(0);
    ResetAudioState(flS);

    // initialize nextReadTime to now (consumer will read immediately)
    {
        std::lock_guard<std::mutex> lk(flS.timingMutex);
        using clock = std::chrono::steady_clock;
        flS.nextReadTime = std::chrono::duration<double>(clock::now().time_since_epoch()).count();
    }

    LOGI("Loaded ViPE manifest: file=%s width=%d height=%d fps=%.6f",
         flC.file.c_str(), flC.width, flC.height, flC.fps);

    return true;
}

bool FrameLoaderSystem::LoadCatalog(FrameLoaderComponent& flC) {
#if defined(ANDROID)
    const std::string catalogLocation = flC.baseUrl + "/catalog.json";
    std::vector<uint8_t> bytes;
    if (!ReadResource(catalogLocation, bytes)) {
        flC.errorMessage = "Failed to load dataset catalog: " + catalogLocation;
        LOGE("%s", flC.errorMessage.c_str());
        return false;
    }
    std::string error;
    if (!ParseVipeCatalog(std::string(bytes.begin(), bytes.end()), flC.catalog, error)) {
        flC.errorMessage = "Invalid dataset catalog: " + error;
        LOGE("%s", flC.errorMessage.c_str());
        return false;
    }
#else
    namespace fs = std::filesystem;
    const fs::path directory(flC.dataDirectory);
    if (!fs::is_directory(directory)) {
        flC.errorMessage = "ViPE data directory does not exist: " + directory.string();
        LOGE("%s", flC.errorMessage.c_str());
        return false;
    }
    flC.catalog = {};
    flC.catalog.schemaVersion = 1;
    for (const fs::directory_entry& entry : fs::directory_iterator(directory)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".json" ||
            entry.path().filename() == "catalog.json") {
            continue;
        }
        VipeCatalogEntry item;
        item.id = entry.path().stem().string();
        item.displayName = item.id;
        item.manifest = entry.path().string();
        flC.catalog.datasets.push_back(std::move(item));
    }
    std::sort(flC.catalog.datasets.begin(), flC.catalog.datasets.end(),
        [](const VipeCatalogEntry& a, const VipeCatalogEntry& b) { return a.id < b.id; });
    if (flC.catalog.datasets.empty()) {
        flC.errorMessage = "No ViPE manifests found in " + directory.string();
        LOGE("%s", flC.errorMessage.c_str());
        return false;
    }
#endif
    return true;
}

bool FrameLoaderSystem::SelectDataset(
    const std::string& datasetId,
    FrameLoaderComponent& flC,
    FrameLoaderState& flS) {
    const auto selected = std::find_if(
        flC.catalog.datasets.begin(), flC.catalog.datasets.end(),
        [&](const VipeCatalogEntry& entry) { return entry.id == datasetId; });
    if (selected == flC.catalog.datasets.end()) {
        flC.errorMessage = "Dataset is not present in catalog: " + datasetId;
        return false;
    }
    const std::string previousSelectedDatasetId = flC.selectedDatasetId;
    const std::string previousManifestLocation = flC.manifestLocation;
    const std::string previousVideoLocation = flC.videoLocation;
    const std::string previousFile = flC.file;
    const int previousWidth = flC.width;
    const int previousHeight = flC.height;
    const double previousFps = flC.fps;
    const float previousDepthScaleFactor = flC.depthScaleFactor;
    const VipeDataset previousDataset = flC.dataset;
    StopBackgroundWriter(flC, flS);
    ResetAudioState(flS);
    for (FrameSlot& slot : flS.ring) {
        slot.ready.store(false, std::memory_order_release);
        slot.frame = nullptr;
    }
    flS.framePtr = nullptr;
    flS.frameReady.store(false, std::memory_order_release);
    flS.readIdx.store(0, std::memory_order_release);
    flS.writeIdx.store(0, std::memory_order_release);
    flC.selectedDatasetId = selected->id;
#if defined(ANDROID)
    if (!selected->manifest.empty() && selected->manifest.front() == '/') {
        flC.manifestLocation = flC.baseUrl + selected->manifest;
    } else {
        flC.manifestLocation = flC.baseUrl + "/" + selected->manifest;
    }
#else
    flC.manifestLocation = selected->manifest;
#endif
    if (!LoadManifest(flC, flS)) {
        const std::string selectionError = flC.errorMessage;
        flC.selectedDatasetId = previousSelectedDatasetId;
        flC.manifestLocation = previousManifestLocation;
        flC.videoLocation = previousVideoLocation;
        flC.file = previousFile;
        flC.width = previousWidth;
        flC.height = previousHeight;
        flC.fps = previousFps;
        flC.depthScaleFactor = previousDepthScaleFactor;
        flC.dataset = previousDataset;
        flC.errorMessage = selectionError;
        if (!previousSelectedDatasetId.empty() && !previousVideoLocation.empty()) {
            StartBackgroundWriter(flC, flS);
        }
        return false;
    }
    StartBackgroundWriter(flC, flS);
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
    std::vector<uint8_t> fetched = LoadVideo(flC);
    if (fetched.empty()) {
        flC.errorMessage = "Video is empty or could not be loaded: " + flC.videoLocation;
        LOGE("%s", flC.errorMessage.c_str());
        flC.writerRunning.store(false, std::memory_order_release);
        return;
    }

    // Create and init demuxer
    WebmInMemoryDemuxer demuxer(fetched);
    std::string error;
    if (!demuxer.init(&error)) {
        LOGE("Failed to init demuxer: %s", error.c_str());
        flC.writerRunning.store(false, std::memory_order_release);
        return;
    }
    LOGI("Demuxer initialized: video %dx%d", demuxer.width(), demuxer.height());
    flS.audioAvailable.store(demuxer.has_audio(), std::memory_order_release);
    flS.audioSampleRate.store(demuxer.audio_sample_rate(), std::memory_order_release);

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
                    ResetAudioState(flS);
                    flS.audioAvailable.store(demuxer.has_audio(), std::memory_order_release);
                    continue;
                }

                std::vector<AudioPcmBlock> audio = demuxer.take_audio_blocks();
                if (!audio.empty()) {
                    std::lock_guard<std::mutex> audioLock(flS.audioMutex);
                    size_t queuedSamples = 0;
                    for (const AudioPcmBlock& block : flS.audioQueue) {
                        queuedSamples += block.samples.size();
                    }
                    for (AudioPcmBlock& block : audio) {
                        if (flS.audioQueue.empty() && queuedSamples == 0 &&
                            flS.audioPlayedFrames.load(std::memory_order_acquire) == 0) {
                            flS.audioEpochUs.store(block.timestampUs, std::memory_order_release);
                        }
                        queuedSamples += block.samples.size();
                        flS.audioQueue.push_back(std::move(block));
                    }
                    while (queuedSamples > MAX_AUDIO_SAMPLES && flS.audioQueue.size() > 1) {
                        queuedSamples -= flS.audioQueue.front().samples.size();
                        flS.audioQueue.pop_front();
                        flS.audioSampleOffset = 0;
                    }
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
std::vector<uint8_t> FrameLoaderSystem::LoadVideo(FrameLoaderComponent& flC) {
    std::vector<uint8_t> blob;
    LOGI("Loading video from %s", flC.videoLocation.c_str());
    if (!ReadResource(flC.videoLocation, blob)) {
        LOGE("Video load failed for %s", flC.videoLocation.c_str());
        return blob;
    }
    return blob;
}

bool FrameLoaderSystem::ReadResource(const std::string& location, std::vector<uint8_t>& out) {
#if defined(ANDROID)
    return HttpGetBinary(location, out);
#else
    std::ifstream input(location, std::ios::binary);
    if (!input) {
        return false;
    }
    input.seekg(0, std::ios::end);
    const std::streamoff size = input.tellg();
    if (size < 0) {
        return false;
    }
    input.seekg(0, std::ios::beg);
    out.resize(static_cast<size_t>(size));
    return size == 0 || static_cast<bool>(input.read(reinterpret_cast<char*>(out.data()), size));
#endif
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
#if defined(ANDROID)
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
#else
    (void)url;
    (void)out;
    return false;
#endif
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
void FrameLoaderSystem::SetFPS(double newFps, FrameLoaderComponent& flC, FrameLoaderState& flS) {
    flC.fps = newFps;
    // adjust nextReadTime to avoid huge skips
    std::lock_guard<std::mutex> lk(flS.timingMutex);
    using clock = std::chrono::steady_clock;
    flS.nextReadTime = std::chrono::duration<double>(clock::now().time_since_epoch()).count();
}

void FrameLoaderSystem::SetPaused(
        bool paused, double nowSeconds,
        FrameLoaderComponent& flC, FrameLoaderState& flS) {
    const bool wasPaused = flC.paused;
    if (wasPaused == paused) return;
    flC.paused = paused;
    std::lock_guard<std::mutex> lock(flS.timingMutex);
    flS.nextReadTime = PlaybackDeadlineAfterPauseChange(
        wasPaused, paused, nowSeconds, flS.nextReadTime);
    LOGI("Video playback %s", paused ? "paused" : "resumed");
}

void FrameLoaderSystem::TogglePaused(
        double nowSeconds, FrameLoaderComponent& flC, FrameLoaderState& flS) {
    SetPaused(!flC.paused, nowSeconds, flC, flS);
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
    double localFps = flC.fps;
    if (localFps <= 0) localFps = 1;
    double period = 1.0 / double(localFps);

    const bool useAudioClock =
        flS.audioAvailable.load(std::memory_order_acquire) &&
        flS.audioStarted.load(std::memory_order_acquire);
    if (!useAudioClock) {
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
    if (useAudioClock) {
        const int sampleRate = std::max(1, flS.audioSampleRate.load(std::memory_order_acquire));
        const int64_t audioTimeUs =
            flS.audioEpochUs.load(std::memory_order_acquire) +
            flS.audioPlayedFrames.load(std::memory_order_acquire) * 1000000LL / sampleRate;
        const int64_t lateThresholdUs = static_cast<int64_t>(period * 1000000.0);
        while (flS.ring[currentReadSlot].ready.load(std::memory_order_acquire)) {
            const VideoFrame* candidate = flS.ring[currentReadSlot].frame;
            if (!candidate || candidate->ts_us >= audioTimeUs - lateThresholdUs) break;
            flS.ring[currentReadSlot].ready.store(false, std::memory_order_release);
            currentReadSlot = (currentReadSlot + 1) % RING_SIZE;
            flS.readIdx.store(currentReadSlot, std::memory_order_release);
            flS.writerCv.notify_one();
        }
        if (!flS.ring[currentReadSlot].ready.load(std::memory_order_acquire)) return false;
        const VideoFrame* candidate = flS.ring[currentReadSlot].frame;
        if (candidate && candidate->ts_us > audioTimeUs + static_cast<int64_t>(period * 500000.0)) {
            return false;
        }
    } else if (!flS.ring[currentReadSlot].ready.load(std::memory_order_acquire)) {
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
