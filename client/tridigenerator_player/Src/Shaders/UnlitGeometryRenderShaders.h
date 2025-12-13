#pragma once

const char* UnlitGeometryVertexShaderSrc = R"glsl(
// Attributes
attribute highp vec4 Position;
attribute highp vec3 Normal;
attribute highp vec2 TexCoord;

uniform usampler2D u_texDepth;
uniform highp float u_FovX_rad; // Horizontal FOV in radians (e.g., fovx_deg * PI / 180.0)
uniform highp float u_FovY_rad; // Vertical FOV in radians (calculated from aspect ratio)
uniform highp float u_depthScaleFactor;


// Outputs to fragment shader
varying lowp vec2 oTexCoord;
varying lowp vec4 oColor;

void main()
{
    // Reconstruct 16-bit depth value
    uint uz = texture(u_texDepth, TexCoord).r;

    // Convert to meters using the factor from the manifest
    float z = float(uz) / u_depthScaleFactor;

    // Calculate X and Y world coordinates using projection math.
    // TexCoord is [0, 1]. Convert to Normalized Device Coordinates [-1, 1].
    float ndc_x = TexCoord.x * 2.0 - 1.0;
    // For Y, texture coordinates often have 0 at the top. We need to flip this
    // so that +Y in screen space maps to +Y in world space.
    float ndc_y = 1.0 - TexCoord.y * 2.0;

    // The tangent of the half-FOV gives the extent of the view plane at distance 1.
    // We multiply by NDC to find the point on that plane, then scale by depth.
    float x = ndc_x * tan(u_FovX_rad) * z;
    float y = ndc_y * tan(u_FovY_rad) * z;

    //float x = ndc_x;
    //float y = ndc_y;

    // The Z coordinate in view space is negative.
    vec4 worldPosition = vec4(x, y, -z, 1.0);

    // Transform from local model space to world/view/clip space
    gl_Position = TransformVertex( worldPosition );
    oTexCoord = TexCoord;
    oColor = vec4(1,1,1,1);
}
)glsl";

static const char* UnlitGeometryFragmentShaderSrc = R"glsl(
uniform sampler2D u_texY;
uniform sampler2D u_texU;
uniform sampler2D u_texV;
uniform sampler2D u_texAlpha;

varying lowp vec2 oTexCoord;
varying lowp vec4 oColor;

vec3 yuv_to_rgb(float y, float u, float v) {
    float c = y - 0.0625;
    float d = u - 0.5;
    float e = v - 0.5;
    float r = 1.1643 * c + 1.5958 * e;
    float g = 1.1643 * c - 0.39173 * d - 0.81290 * e;
    float b = 1.1643 * c + 2.017 * d;
    return vec3(r, g, b);
}

void main()
{
    // Get alpha value from its own texture
    float alpha = texture(u_texAlpha, oTexCoord).r;

    // Discard fragment if alpha is below a threshold
    if (alpha < 0.5) discard;

    // Get color value from YUV textures
    float y = texture(u_texY, oTexCoord).r;
    float u = texture(u_texU, oTexCoord).r;
    float v = texture(u_texV, oTexCoord).r;
    vec3 rgb = yuv_to_rgb(y, u, v);

    gl_FragColor.xyz = rgb;
    gl_FragColor.w = 1.0;
}
)glsl";
