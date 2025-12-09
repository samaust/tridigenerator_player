#pragma once

/**
 * Struct returned for each decoded frame.
 * All planes are tightly packed (row stride == width for each plane).
 */
struct VideoFrame {
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

    int64_t ts_us = 0;

    // Ownership: these internal buffers belong to WebmInMemoryDemuxer
    // and remain valid until the next call to decode_next_frame().
};
