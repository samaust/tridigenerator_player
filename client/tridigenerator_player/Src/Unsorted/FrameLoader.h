#pragma once
#include <vector>
#include <string>
#include <GLES3/gl3.h>

#include "OVR_Math.h"

struct FrameInfo {
    std::string file;
};

struct FrameData {
    std::vector<OVR::Vector3f> position;
    std::vector<OVR::Vector4f> color;
    //std::vector<float> classes;
};

class FrameLoader {
public:
    FrameLoader() : baseUrl("") {}
    FrameLoader(const std::string& baseUrl);

    int height;
    int width;
    int spawned;

    bool loadManifest();
    bool loadFrame(int idx);
    int getNumFrames() const { return (int)frames.size(); }
    FrameData& getCurrentFrame() { return frame; }

private:
    std::string baseUrl;
    std::vector<FrameInfo> frames;
    FrameData frame;

    bool httpGet(const std::string& url, std::string& out);
    bool httpGetBinary(const std::string& url, std::vector<uint8_t>& out);
};
