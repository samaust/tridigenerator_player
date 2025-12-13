#pragma once
#include <GLES3/gl3.h>

/// Returns the number of bytes per pixel for a given OpenGL pixel format.
/// @param fmt The GLenum pixel format (e.g., GL_R8, GL_RGBA8).
/// @return The number of bytes per pixel for the format. Returns 0 for unsupported formats.
/// This function is designed to be used at compile time.
constexpr uint8_t bytesPerPixel(GLenum fmt) {
    switch (fmt) {
        // 1 byte
        case GL_R8: return 1;
        case GL_R8I: return 1;
        case GL_R8UI: return 1;

        // 2 bytes
        case GL_R16F: return 2;
        case GL_R16I: return 2;
        case GL_R16UI: return 2;
        case GL_RG8: return 2;
        case GL_RG8I: return 2;
        case GL_RG8UI: return 2;

        // 3 bytes
        case GL_RGB8: return 3; // rarely used in practice

        // 4 bytes
        case GL_R32F: return 4;
        case GL_R32I: return 4;
        case GL_R32UI: return 4;
        case GL_RG16F: return 4;
        case GL_RG16I: return 4;
        case GL_RG16UI: return 4;
        case GL_RGBA8: return 4;
        case GL_RGBA8I: return 4;
        case GL_RGBA8UI: return 4;

        // 6 bytes
        case GL_RGB16F: return 6;
        case GL_RGB16I: return 6;
        case GL_RGB16UI: return 6;

        // 8 bytes
        case GL_RG32F: return 8;
        case GL_RG32I: return 8;
        case GL_RG32UI: return 8;
        case GL_RGBA16F: return 8;
        case GL_RGBA16I: return 8;
        case GL_RGBA16UI: return 8;

        // 12 bytes
        case GL_RGB32F: return 12;
        case GL_RGB32I: return 12;
        case GL_RGB32UI: return 12;

        // 16 bytes
        case GL_RGBA32F: return 16;
        case GL_RGBA32I: return 16;
        case GL_RGBA32UI: return 16;
    }
    return 0; // unreachable in your use-case
}


/// Computes the GL_UNPACK_ALIGNMENT value from the stride in bytes.
/// OpenGL requires that the start of each row of texture data is aligned to a specific byte boundary.
/// This function determines the largest power of 2 (up to 8) that the row stride is divisible by.
/// @param rowStrideBytes The stride of a row of texture data in bytes.
/// @return The required alignment value (1, 2, 4, or 8) for glPixelStorei(GL_UNPACK_ALIGNMENT, ...).
/// This function is designed to be used at compile time.
constexpr GLint computeUnpackAlignment(uint32_t rowStrideBytes)
{
    if (rowStrideBytes % 8 == 0) return 8;
    if (rowStrideBytes % 4 == 0) return 4;
    if (rowStrideBytes % 2 == 0) return 2;
    return 1;
}