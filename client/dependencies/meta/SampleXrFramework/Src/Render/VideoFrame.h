#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

#include "DynamicCellCullingData.h"

/**
 * Struct returned for each decoded frame.
 * All planes are tightly packed (row stride == width for each plane).
 */
struct VideoFrame {
    struct PlaneView {
        const uint8_t* data = nullptr;
        uint32_t width = 0;
        uint32_t height = 0;
        int stride = 0;
    };
    int frameIndex = -1;
    // --- COLOR DATA (YUV) ---
    std::vector<uint8_t> textureYData;
    uint32_t textureYWidth = 0;
    uint32_t textureYHeight = 0;
    int textureYStride = 0;
    std::vector<uint8_t> textureUData;
    uint32_t textureUWidth = 0;
    uint32_t textureUHeight = 0;
    int textureUStride = 0;
    std::vector<uint8_t> textureVData;
    uint32_t textureVWidth = 0;
    uint32_t textureVHeight = 0;
    int textureVStride = 0;
    // Keeps decoder-owned color planes alive until this ring entry has been
    // uploaded. This avoids a decoder-thread Y/U/V copy.
    std::shared_ptr<void> colorPlaneOwner;
    std::array<PlaneView, 3> colorPlaneViews{};
    // Android MediaCodec output. The renderer imports the AImage's private
    // hardware buffer and releases this owner after its GL fence.
    std::shared_ptr<void> hardwareColorImage;

    // --- ALPHA DATA ---
    std::vector<uint8_t> textureAlphaData;
    uint32_t textureAlphaWidth = 0;
    uint32_t textureAlphaHeight = 0;
    int textureAlphaStride = 0;

    // --- DEPTH DATA (16-bit) ---
    std::vector<uint16_t> textureDepthData;
    uint32_t textureDepthWidth = 0;
    uint32_t textureDepthHeight = 0;
    int textureDepthStride = 0;
    // Android retains the FFmpeg frame until the writer has sampled and
    // endian-converted the pixels needed by the active mesh.
    std::shared_ptr<void> depthPlaneOwner;
    PlaneView depthPlaneView;
    bool depthPlaneBigEndian = false;

    // Depth resized and bounded by the decoder thread for the active mesh detail.
    std::vector<uint16_t> preparedDepthData;
    uint32_t preparedDepthWidth = 0;
    uint32_t preparedDepthHeight = 0;
    bool preparedDepthBoundsValid = false;
    std::array<float, 3> preparedDepthBoundsMinimum{};
    std::array<float, 3> preparedDepthBoundsMaximum{};
    DynamicCellCullingData dynamicCellCulling;

    int64_t ts_us = 0;
    bool yuvFullRange = false;

    // Ownership: these internal buffers belong to WebmInMemoryDemuxer
    // and remain valid until the next call to decode_next_frame().
};
