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

bool FrameLoader::HttpGet(const std::string& url, std::string& out) {
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

bool FrameLoader::HttpGetBinary(const std::string& url, std::vector<uint8_t>& out) {
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

bool FrameLoader::LoadManifest() {
    std::string url = baseUrl + "/manifest/frames.json";
    std::string jsonStr;
    if (!HttpGet(url, jsonStr)) {
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

FrameData FrameLoader::LoadFrameFromIndex(int idx) {
    if (idx < 0 || idx >= (int)frames.size()) {
        FrameData frame;

        return frame;
    }

    std::string url = baseUrl + "/frames/" + frames[idx].file;

    return LoadFrameFromUrl(url);
}

FrameData FrameLoader::LoadFrameFromUrl(const std::string& url) {
    std::vector<uint8_t> blob;
    if (!HttpGetBinary(url, blob))
    {
        throw std::runtime_error("HttpGetBinary failed: " + url);
    }

    return ParseFrame(blob);
}

FrameData FrameLoader::ParseFrame(const std::vector<uint8_t>& blob)
{
    FrameData frame;

    const uint8_t* ptr = blob.data();
    const uint8_t* end = blob.data() + blob.size();

    auto require = [&](size_t bytes) {
        if (ptr + bytes > end)
            throw std::runtime_error("Corrupt or truncated frame data");
    };

    //
    // Header (12 bytes)
    //
    require(12);
    std::memcpy(&frame.width,      ptr + 0, 4);
    std::memcpy(&frame.height,     ptr + 4, 4);
    std::memcpy(&frame.pointCount, ptr + 8, 4);
    ptr += 12;

    if (frame.pointCount != frame.width * frame.height)
        throw std::runtime_error("pointCount mismatch with width * height");

    //
    // XYZ float32 (pointCount × 3)
    //
    const size_t xyzFloats = size_t(frame.pointCount) * 3;
    const size_t xyzBytes  = xyzFloats * sizeof(float);

    require(xyzBytes);

    frame.positions.resize(frame.pointCount);
    std::memcpy(frame.positions.data(), ptr, xyzBytes);
    ptr += xyzBytes;

    //
    // RGBA float32 (pointCount × 4)
    //
    const size_t rgbaFloats = size_t(frame.pointCount) * 4;
    const size_t rgbaBytes  = rgbaFloats * sizeof(float);

    require(rgbaBytes);

    frame.colors.resize(frame.pointCount);
    std::memcpy(frame.colors.data(), ptr, rgbaBytes);
    ptr += rgbaBytes;

    return frame;
}
