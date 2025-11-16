#include "FrameLoader.h"

#include <android/log.h>
#include <curl/curl.h>
#include <json/json.h>

#include <chrono>
#include <cstring>
#include <stdexcept>

static double NowMs() {
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "FrameLoader", __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "FrameLoader", __VA_ARGS__)

// ---------- You already have writeBinary / writeString helpers for curl ----------
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
FrameLoader::FrameLoader(const std::string& baseUrl_) : baseUrl(baseUrl_) {
    curl_global_init(CURL_GLOBAL_ALL);

    // initialize ring
    ring.resize(RING_SIZE);
    // writer not started yet
}

FrameLoader::~FrameLoader() {
    StopBackgroundWriter();
    curl_global_cleanup();
}

// ---------- Manifest loader (fixed fps read) ----------
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

    if (root.isMember("width") && root["width"].isIntegral()) {
        width = root["width"].asInt();
    }
    if (root.isMember("height") && root["height"].isIntegral()) {
        height = root["height"].asInt();
    }
    if (root.isMember("fps") && root["fps"].isIntegral()) {
        fps.store(root["fps"].asInt(), std::memory_order_relaxed);
    }

    frames.clear();
    const Json::Value arr = root["frames"];
    for (const auto& el : arr) {
        FrameInfo fi;
        fi.file = el["file"].asString();
        frames.push_back(fi);
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

    LOGI("Loaded manifest: frames=%zu width=%d height=%d fps=%d",
         frames.size(), width, height, fps.load());
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

// ---------- ParseFrame (same format: header + XYZ float32 + RGBA float32) ----------
FrameData FrameLoader::ParseFrame(const std::vector<uint8_t>& blob) {
    FrameData frame;
    const uint8_t* ptr = blob.data();
    const uint8_t* end = ptr + blob.size();

    auto require = [&](size_t bytes) {
        if (ptr + bytes > end) throw std::runtime_error("Corrupt or truncated frame data");
    };

    require(12);
    std::memcpy(&frame.width, ptr + 0, 4);
    std::memcpy(&frame.height, ptr + 4, 4);
    std::memcpy(&frame.pointCount, ptr + 8, 4);
    ptr += 12;

    if (frame.pointCount != frame.width * frame.height) throw std::runtime_error("pointCount mismatch");

    const size_t xyzFloats = size_t(frame.pointCount) * 3;
    const size_t xyzBytes = xyzFloats * sizeof(float);
    require(xyzBytes);
    frame.positions.resize(frame.pointCount);
    std::memcpy(frame.positions.data(), ptr, xyzBytes);
    ptr += xyzBytes;

    const size_t rgbaFloats = size_t(frame.pointCount) * 4;
    const size_t rgbaBytes = rgbaFloats * sizeof(float);
    require(rgbaBytes);
    frame.colors.resize(frame.pointCount);
    std::memcpy(frame.colors.data(), ptr, rgbaBytes);
    ptr += rgbaBytes;

    return frame;
}

// ---------- LoadFrameFromIndex ----------
FrameData FrameLoader::LoadFrameFromIndex(int idx) {
    const double t_0 = NowMs();
    FrameData empty;
    if (idx < 0 || idx >= (int)frames.size()) return empty;

    std::string url = baseUrl + "/frames/" + frames[idx].file;
    std::vector<uint8_t> blob;
    if (!HttpGetBinary(url, blob)) {
        LOGE("HttpGetBinary failed for %s", url.c_str());
        return empty;
    }
    const double t_http = NowMs();
    try {
        FrameData f = ParseFrame(blob);
        const double t_PARSING = NowMs();
        LOGI( "  HTTP only:     %.3f ms", t_http - t_0 );
        LOGI( "  PARSING only:     %.3f ms", t_PARSING - t_http );
        return f;
    } catch (const std::exception& e) {
        LOGE("ParseFrame failed: %s", e.what());
        return empty;
    }
}

// ---------- ComputeFreeSlots ----------
int FrameLoader::ComputeFreeSlots() const {
    int w = writeIdx.load(std::memory_order_acquire);
    int r = readIdx.load(std::memory_order_acquire);
    int occupied = (w - r + RING_SIZE) % RING_SIZE; // number of occupied slots
    int freeSlots = RING_SIZE - occupied - 1; // reserve one slot to disambiguate
    if (freeSlots < 0) freeSlots = 0;
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
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }

        // Optionally limit how many frames we'll prefetch per loop to avoid hogging network
        int toFetch = std::min(freeSlots, TARGET_FILL);

        // But also fetch at least one if empty buffer
        if (toFetch <= 0) toFetch = 1;

        for (int i = 0; i < toFetch && writerRunning.load(std::memory_order_relaxed); ++i) {
            // Before fetching, check again to avoid racing with reader
            freeSlots = ComputeFreeSlots();
            if (freeSlots == 0) break;

            int slot = writeIdx.load(std::memory_order_acquire);
            FrameSlot& s = ring[slot];

            // Sanity: only write to a slot that is not ready
            if (!s.ready.load(std::memory_order_acquire)) {
                // Determine manifest index to fetch
                int mIdx = manifestFetchIdx.fetch_add(1, std::memory_order_acq_rel);
                if (mIdx >= (int)frames.size()) {
                    if (looping.load(std::memory_order_acquire)) {
                        // wrap
                        manifestFetchIdx.store(0, std::memory_order_release);
                        mIdx = 0;
                    } else {
                        // no more frames to fetch
                        writerRunning.store(false, std::memory_order_release);
                        break;
                    }
                }

                const double t_0 = NowMs();
                // Fetch frame (network)
                FrameData fetched = LoadFrameFromIndex(mIdx);
                const double t_http = NowMs();

                // Move into slot
                s.data = std::move(fetched); // cheap move => transfers vectors
                // Mark ready
                s.ready.store(true, std::memory_order_release);

                // Advance write index
                writeIdx.store((slot + 1) % RING_SIZE, std::memory_order_release);
                const double t_store = NowMs();
                LOGI( "  HTTP+PARSING:     %.3f ms", t_http - t_0 );
                LOGI( "  Store:   %.3f ms", t_store - t_http );
            } else {
                // Slot unexpectedly still ready; avoid overwriting it
                // Sleep and retry
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        }

        // If still running, small sleep to avoid busy spin
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } // while
    LOGI("Writer thread exiting");
}

// ---------- ReadFrameIfNeeded (fast for Update) ----------
bool FrameLoader::ReadFrameIfNeeded(double nowSeconds) {
    // Compute frame period
    int localFps = fps.load(std::memory_order_relaxed);
    if (localFps <= 0) localFps = 1;
    double period = 1.0 / double(localFps);

    // Quick check without locking: if now < nextReadTime -> nothing to do
    {
        std::lock_guard<std::mutex> lk(timingMutex);
        if (nowSeconds < nextReadTime) return false;
    }

    // It's time to consume a frame. Check slot at readIdx
    int slot = readIdx.load(std::memory_order_acquire);
    FrameSlot& s = ring[slot];

    if (!s.ready.load(std::memory_order_acquire)) {
        // No new frame to consume; advance nextReadTime to avoid spinning
        std::lock_guard<std::mutex> lk(timingMutex);
        nextReadTime += period; // skip this tick (keeps playback schedule)
        return false;
    }

    // Move slot.data into currentFrame (O(1) move of vectors)
    {
        currentFrame = std::move(s.data);
        // Clear s.data to allow writer to reuse memory without surprises
        s.data = FrameData();
        // Mark slot free
        s.ready.store(false, std::memory_order_release);
        // Advance read index
        readIdx.store((slot + 1) % RING_SIZE, std::memory_order_release);
    }

    // Advance nextReadTime by one period (keep monotonic schedule)
    {
        std::lock_guard<std::mutex> lk(timingMutex);
        // If nextReadTime was in the past by multiple periods, catch up to now + small slack
        nextReadTime += period;
        // Prevent drift: ensure nextReadTime > nowSeconds
        if (nextReadTime <= nowSeconds) nextReadTime = nowSeconds + period;
    }

    // Notify writer that space is available
    writerCv.notify_one();

    return true;
}

void FrameLoader::SetFPS(int newFps) {
    fps.store(newFps, std::memory_order_relaxed);
    // adjust nextReadTime to avoid huge skips
    std::lock_guard<std::mutex> lk(timingMutex);
    using clock = std::chrono::steady_clock;
    nextReadTime = std::chrono::duration<double>(clock::now().time_since_epoch()).count();
}
