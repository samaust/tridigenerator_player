#pragma once
#include <vector>
#include <string>
#include <GLES3/gl3.h>

#include "OVR_Math.h"

struct FrameInfo {
    std::string file;
};

struct FrameData {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t pointCount = 0;

    std::vector<OVR::Vector3f> positions; // XYZ
    std::vector<OVR::Vector4f> colors;    // RGBA
};

class FrameLoader {
public:
    FrameLoader() : baseUrl("") {}
    FrameLoader(const std::string& baseUrl);

    int height;
    int width;
    int spawned;

    bool LoadManifest();
    FrameData LoadFrameFromIndex(int idx);
    FrameData LoadFrameFromUrl(const std::string& url);
    int GetNumFrames() const { return (int)frames.size(); }
    FrameData& GetCurrentFrame() { return frame; }

private:
    std::string baseUrl;
    std::vector<FrameInfo> frames;
    FrameData frame;

    bool HttpGet(const std::string& url, std::string& out);
    bool HttpGetBinary(const std::string& url, std::vector<uint8_t>& out);
    static FrameData ParseFrame(const std::vector<uint8_t>& blob);
};
