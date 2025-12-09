#include "FrameLoader.h"

#include <android/log.h>
#include <curl/curl.h>
#include <json/json.h>

#include <chrono>
#include <cstring>
#include <stdexcept>
#include <inttypes.h>

/*
static double NowMs() {
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}
*/

#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "FrameLoader", __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "FrameLoader", __VA_ARGS__)

// ---------- writeBinary / writeString helpers for curl ----------
static size_t writeString(void* ptr, size_t size, size_t nmemb, void* userdata) {
    std::string* str = reinterpret_cast<std::string*>(userdata);
    size_t total = size * nmemb;
    str->append(reinterpret_cast<char*>(ptr), total);
    return total;
}
static size_t writeBinary(void* ptr, size_t size, size_t nmemb, void* userdata) {
    std::vector<uint8_t>* data = reinterpret_cast<std::vector<uint8_t>*>(userdata);
    size_t total = size * nmemb;
    uint8_t* start = reinterpret_cast<uint8_t*>(ptr);
    data->insert(data->end(), start, start + total);
    return total;
}

// ---------- Constructor / destructor ----------
FrameLoader::FrameLoader(const std::string& baseUrl_)
    : baseUrl(baseUrl_),
      framePool_(RING_SIZE), // Initialize framePool_ with RING_SIZE elements
      ring(RING_SIZE)         // Initialize ring with RING_SIZE default-initialized FrameSlots
{
    curl_global_init(CURL_GLOBAL_ALL);
}

FrameLoader::~FrameLoader() {
    StopBackgroundWriter();
    curl_global_cleanup();
}

// ---------- Manifest loader ----------
bool FrameLoader::LoadManifest() {
    std::string url = baseUrl + "/manifest/frames.json";
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
        file = root["file"].asString();
    }
    if (root.isMember("width") && root["width"].isIntegral()) {
        width = root["width"].asInt();
    }
    if (root.isMember("height") && root["height"].isIntegral()) {
        height = root["height"].asInt();
    }
    if (root.isMember("fps") && root["fps"].isIntegral()) {
        fps.store(root["fps"].asInt(), std::memory_order_relaxed);
    }
    if (root.isMember("depth_scale_factor") && root["depth_scale_factor"].isNumeric()) {
        depthScaleFactor = root["depth_scale_factor"].asFloat();
    }

    // reset indices
    manifestFetchIdx.store(0);
    writeIdx.store(0);
    readIdx.store(0);

    // initialize nextReadTime to now (consumer will read immediately)
    {
        std::lock_guard<std::mutex> lk(timingMutex);
        using clock = std::chrono::steady_clock;
        nextReadTime = std::chrono::duration<double>(clock::now().time_since_epoch()).count();
    }

    LOGI("Loaded manifest: file=%s width=%d height=%d fps=%d",
         file.c_str(), width, height, fps.load());
    return true;
}

// ---------- HttpGetBinary (curl) ----------
bool FrameLoader::HttpGetBinary(const std::string& url, std::vector<uint8_t>& out) {
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

// ---------- LoadVideoFromIndex ----------
std::vector<uint8_t> FrameLoader::LoadVideoFromIndex(int idx) {
    std::vector<uint8_t> blob;

    std::string url = baseUrl + "/frames/" + file.c_str();
    LOGI("Loading video frame %d from %s", idx, url.c_str());
    if (!HttpGetBinary(url, blob)) {
        LOGE("HttpGetBinary failed for %s", url.c_str());
        return blob;
    }
    return blob;
}

// ---------- ComputeFreeSlots ----------
int FrameLoader::ComputeFreeSlots() const {
    int w = writeIdx.load(std::memory_order_acquire);
    int r = readIdx.load(std::memory_order_acquire);

    // This is the classic algorithm for calculating used space in a circular buffer.
    int usedSlots = (w - r + RING_SIZE) % RING_SIZE;

    // The number of free slots is the total size minus what's used.
    // We must always leave at least one slot empty to distinguish a full buffer
    // from an empty one (where w == r).
    int freeSlots = RING_SIZE - usedSlots - 1;

    return freeSlots;
}


// ---------- Writer control ----------
void FrameLoader::StartBackgroundWriter() {
    bool expected = false;
    if (!writerRunning.compare_exchange_strong(expected, true)) return; // already running
    writerThread = std::thread(&FrameLoader::WriterLoop, this);
}

void FrameLoader::StopBackgroundWriter() {
    bool expected = true;
    if (!writerRunning.compare_exchange_strong(expected, false)) {
        // not running
        return;
    }
    writerCv.notify_all();
    if (writerThread.joinable()) writerThread.join();
}

// ---------- WriterLoop ----------
void FrameLoader::WriterLoop() {
    LOGI("Writer thread started");
    const int TARGET_FILL = RING_SIZE / 2; // keep half-filled
    int mIdx = manifestFetchIdx.load(std::memory_order_relaxed);

    // Download video
    //const double t_0 = NowMs();
    // Fetch frame (network)
    std::vector<uint8_t> fetched = LoadVideoFromIndex(mIdx);
    //const double t_http = NowMs();

    // Create and init demuxer
    WebmInMemoryDemuxer demuxer(fetched);
    std::string error;
    if (!demuxer.init(&error)) {
        LOGE("Failed to init demuxer: %s", error.c_str());
        writerRunning.store(false, std::memory_order_release);
        return;
    }
    LOGI("Demuxer initialized: video %dx%d", demuxer.width(), demuxer.height());

    while (writerRunning.load(std::memory_order_relaxed)) {
        // Wait until there is at least one free slot (or stop)
        {
            std::unique_lock<std::mutex> lk(writerMutex);
            writerCv.wait_for(lk, std::chrono::milliseconds(10), [&]() {
                return !writerRunning.load(std::memory_order_relaxed) || ComputeFreeSlots() > 0;
            });
        }
        if (!writerRunning.load(std::memory_order_relaxed)) break;

        // Re-check free slots
        int freeSlots = ComputeFreeSlots();
        if (freeSlots == 0) {
            // nothing to do
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }

        // Optionally limit how many frames we'll prefetch per loop to avoid hogging network
        int toFetch = std::min(freeSlots, TARGET_FILL);

        for (int i = 0; i < toFetch && writerRunning.load(std::memory_order_relaxed); ++i) {
            // Before fetching, check again to avoid racing with reader
            freeSlots = ComputeFreeSlots();
            if (freeSlots == 0) break;

            int slot = writeIdx.load(std::memory_order_acquire);
            FrameSlot& s = ring[slot];

            // Sanity: only write to a slot that is not ready
            if (!s.ready.load(std::memory_order_acquire)) {
                // Get a pointer to the frame we will write into.
                s.frame = &framePool_[slot];

                // Decode the next frame, passing our target buffer.
                if (!demuxer.decode_next_frame(*(s.frame))) {
                    // End of stream

                    // Check looping
                    if (!looping.load(std::memory_order_acquire)) {
                        // stop writer
                        writerRunning.store(false, std::memory_order_release);
                        break;
                    }

                    // seek to start
                    if (!demuxer.seek_to_start()) {
                        LOGI("seek_to_start() failed.");
                        // stop writer
                        writerRunning.store(false, std::memory_order_release);
                        break;
                    }
                    continue;
                }

                // LOGI("Writer decoded frame ts=%" PRId64 "\n", frame.ts_us);

                // Mark ready (we don't move data anymore)
                s.ready.store(true, std::memory_order_release);

                // Advance write index
                writeIdx.store((slot + 1) % RING_SIZE, std::memory_order_release);
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

void FrameLoader::SetFPS(int newFps) {
    fps.store(newFps, std::memory_order_relaxed);
    // adjust nextReadTime to avoid huge skips
    std::lock_guard<std::mutex> lk(timingMutex);
    using clock = std::chrono::steady_clock;
    nextReadTime = std::chrono::duration<double>(clock::now().time_since_epoch()).count();
}

bool FrameLoader::SwapNextFrame(double nowSeconds, VideoFrame** outFramePtr) {
    // 1. Perform timing checks to see if we should advance to a new frame.
    int localFps = fps.load(std::memory_order_relaxed);
    if (localFps <= 0) localFps = 1;
    double period = 1.0 / double(localFps);

    {
        std::lock_guard<std::mutex> lk(timingMutex);
        if (nowSeconds < nextReadTime) {
            return false; // Not time to show the next frame yet.
        }
        // It's time. Schedule the next frame presentation time.
        nextReadTime += period;
        // Prevent drift if we're lagging: ensure nextReadTime > nowSeconds
        if (nextReadTime <= nowSeconds) {
            nextReadTime = nowSeconds + period;
        }
    }

    // Check if the next slot to be read has data ready from the writer.
    int currentReadSlot = readIdx.load(std::memory_order_acquire);
    if (!ring[currentReadSlot].ready.load(std::memory_order_acquire)) {
        // The writer hasn't produced a new frame for us yet.
        return false;
    }

    // The slot is ready. Give the caller a pointer to this frame's data.
    *outFramePtr = ring[currentReadSlot].frame;

    // IMPORTANT: Mark the slot as "no longer ready for reading by the main thread"
    //            and advance the read pointer. This is the atomic "consumption" step.
    ring[currentReadSlot].ready.store(false, std::memory_order_release);
    readIdx.store((currentReadSlot + 1) % RING_SIZE, std::memory_order_release);

    // Notify the writer thread that a slot has become free.
    writerCv.notify_one();

    // Return true to signal that a new frame has been "consumed" and *outFramePtr is valid.
    return true;
}
