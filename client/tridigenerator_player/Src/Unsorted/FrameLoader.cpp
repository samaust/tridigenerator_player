#include "FrameLoader.h"
#include <android/log.h>
#include <curl/curl.h>
#include <json/json.h>

#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "FrameLoader", __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "FrameLoader", __VA_ARGS__)

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

FrameLoader::FrameLoader(const std::string& baseUrl_) : baseUrl(baseUrl_) {
    curl_global_init(CURL_GLOBAL_ALL);
    spawned = false;
}

bool FrameLoader::httpGet(const std::string& url, std::string& out) {
    CURL* curl = curl_easy_init();
    if (!curl) return false;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeString);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &out);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    return (res == CURLE_OK);
}

bool FrameLoader::httpGetBinary(const std::string& url, std::vector<uint8_t>& out) {
    CURL* curl = curl_easy_init();
    if (!curl) return false;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeBinary);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &out);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    return (res == CURLE_OK);
}

bool FrameLoader::loadManifest() {
    std::string url = baseUrl + "/manifest/frames.json";
    std::string jsonStr;
    if (!httpGet(url, jsonStr)) {
        LOGE("Failed GET %s", url.c_str());
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
    } else {
        //LOGW("Manifest JSON missing or invalid 'width'");
        // You might want to return false or set a default value here
    }

    if (root.isMember("height") && root["height"].isIntegral()) {
        height = root["height"].asInt();
    } else {
        //LOGW("Manifest JSON missing or invalid 'height'");
        // You might want to return false or set a default value here
    }

    frames.clear();
    const Json::Value arr = root["frames"];
    for (const auto& el : arr) {
        FrameInfo fi;
        fi.file = el["file"].asString();
        frames.push_back(fi);
    }

    return true;
}

bool FrameLoader::loadFrame(int idx) {
    if (idx < 0 || idx >= (int)frames.size()) return false;

    std::string url = baseUrl + "/frames/" + frames[idx].file;
    std::vector<uint8_t> buf;
    if (!httpGetBinary(url, buf)) {
        LOGE("Failed GET %s", url.c_str());
        return false;
    }

    // --- Data Structure Constants ---
    // Header: 3 little-endian unsigned integers (width, height, n)
    const size_t headerSize = 3 * sizeof(unsigned int); // 12 bytes
    const int strideF = 4 * 3; // 12 bytes (for X, Y, Z floats)
    const int strideUB = 1 * 4; // 4 bytes (for R, G, B, Class uint8_t)
    int numVerts = width * height; // numVerts is defined by width * height

    // --- Safety Check 1: Total Buffer Size ---
    const size_t floatDataSize = (size_t)numVerts * strideF;
    const size_t byteDataSize = (size_t)numVerts * strideUB;

    // Total Expected Size = Header Size + Float Data Size + Byte Data Size
    const size_t totalExpectedSize = headerSize + floatDataSize + byteDataSize;

    if (buf.size() != totalExpectedSize) {
        LOGE("Data size mismatch! Expected %zu bytes, got %zu bytes.", totalExpectedSize, buf.size());
        return false;
    }
    // --- End Safety Check 1 ---

    // --- Safety Check 2: Header Minimum Size ---
    if (buf.size() < headerSize) {
        LOGE("Buffer is too small to even contain the header.");
        return false;
    }
    // --- End Safety Check 2 ---

    frame.position.resize(numVerts);
    frame.color.resize(numVerts);

    // The start of the float data is *after* the header.
    const uint8_t* floatDataStart = buf.data() + headerSize;

    // The offset to start reading the unsigned bytes is after the header AND all float data.
    const size_t strideUB_offset = headerSize + floatDataSize;
    const uint8_t* bufEnd = buf.data() + buf.size();


    for (int i = 0; i < numVerts; ++i) {
        // Calculate pointers for the current iteration
        const uint8_t* bf = floatDataStart + (size_t)i * strideF;
        const uint8_t* bu = buf.data() + strideUB_offset + (size_t)i * strideUB;

        // --- Safety Check 3: Pointer Bounds Check (inside the loop) ---
        // Check float data bounds
        if (bf + strideF > bufEnd || bf < buf.data()) {
            LOGE("Read out of bounds detected for float data at index %d.", i);
            return false;
        }

        // Check byte data bounds
        if (bu + strideUB > bufEnd || bu < buf.data()) {
            LOGE("Read out of bounds detected for byte data at index %d.", i);
            return false;
        }
        // --- End Safety Check 3 ---

        // Cast and read float data (safe now)
        const float* f = reinterpret_cast<const float*>(bf);
        frame.position[i].x = f[0];
        frame.position[i].y = f[1];
        frame.position[i].z = f[2];

        // Read unsigned byte data (safe now)
        frame.color[i].x = static_cast<float>(bu[0]) / 255.0f;
        frame.color[i].y = static_cast<float>(bu[1]) / 255.0f;
        frame.color[i].z = static_cast<float>(bu[2]) / 255.0f;   
        if (bu[3] == 7) {
            // Mark classification Invisible points as transparent
            frame.color[i].w = 0.0f;
        } else
            frame.color[i].w = 1.0f;
    }
    
    return true;
}