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
varying highp vec4 worldPosition;
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
	highp vec4 localPosition = vec4(x, y, -z, 1.0);
	worldPosition = ModelMatrix * localPosition;

	// Transform from local model space to world/view/clip space
	gl_Position = TransformVertex( localPosition );
	oTexCoord = TexCoord;
	oColor = vec4(1,1,1,1);
}
)glsl";

static const char* UnlitGeometryFragmentShaderSrc = R"glsl(
#ifndef DISABLE_MULTIVIEW
#define DISABLE_MULTIVIEW 0
#endif
#define NUM_VIEWS 2
#if defined( GL_OVR_multiview2 ) && ! DISABLE_MULTIVIEW
    #extension GL_OVR_multiview2 : require
    #define VIEW_ID gl_ViewID_OVR
#else
    uniform lowp int ViewID;
	#define VIEW_ID ViewID
#endif

uniform sampler2D u_texY;
uniform sampler2D u_texU;
uniform sampler2D u_texV;
	uniform sampler2D u_texAlpha;
	uniform lowp int u_hasEnvironmentDepth;
	uniform lowp int u_softOcclusion;
	uniform highp mat4 u_depthViewMatrix[NUM_VIEWS];
	uniform highp mat4 u_depthProjectionMatrix[NUM_VIEWS];
	uniform highp sampler2DArray u_environmentDepthTexture;
	uniform highp vec2 u_environmentDepthTexelSize;
	uniform highp float u_occlusionSoftness;
	uniform highp float u_occlusionDepthBias;

varying lowp vec2 oTexCoord;
varying lowp vec4 oColor;
varying highp vec4 worldPosition;

vec3 yuv_to_rgb(float y, float u, float v) {
    float c = y - 0.0625;
    float d = u - 0.5;
    float e = v - 0.5;
    float r = 1.1643 * c + 1.5958 * e;
    float g = 1.1643 * c - 0.39173 * d - 0.81290 * e;
    float b = 1.1643 * c + 2.017 * d;
    return vec3(r, g, b);
}

#define TransformDepthVertex(localPos) (u_depthProjectionMatrix[VIEW_ID] * ( u_depthViewMatrix[VIEW_ID] * localPos ))

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

	// Environment Depth
	if (u_hasEnvironmentDepth == 0) {
	    gl_FragColor = vec4(rgb, 1.0);
	    return;
    }

	// Transform from world space to depth camera space using 6-DOF matrix
	highp vec4 objectDepthCameraPosition = TransformDepthVertex(worldPosition);

    // 3D point --> Homogeneous Coordinates --> Normalized Coordinates in [0,1]
    if (objectDepthCameraPosition.w <= 0.0) {
        gl_FragColor = vec4(rgb, 1.0);
        return;
    }
    highp vec2 objectDepthCameraPositionHC = objectDepthCameraPosition.xy / objectDepthCameraPosition.w;
    objectDepthCameraPositionHC = objectDepthCameraPositionHC * 0.5f + 0.5f;

    // If the virtual point projects outside the depth image, the depth sample is undefined.
    // Render without occlusion to avoid the entire surface disappearing during head turns.
    if (any(lessThan(objectDepthCameraPositionHC, vec2(0.0))) ||
          any(greaterThan(objectDepthCameraPositionHC, vec2(1.0)))) {
        gl_FragColor = vec4(rgb, 1.0);
        return;
    }

    // Sample from Environment Depth API texture
    highp vec3 depthViewCoord = vec3(objectDepthCameraPositionHC, VIEW_ID);
    highp float depthViewEyeZ = texture(u_environmentDepthTexture, depthViewCoord).r;

    // Get virtual object depth
    highp float objectDepth = objectDepthCameraPosition.z / objectDepthCameraPosition.w;
    objectDepth = objectDepth * 0.5f + 0.5f;

    // Reject invalid depth samples (commonly 0 or 1) to avoid false occlusion blocks.
    if (depthViewEyeZ <= 0.0 || depthViewEyeZ >= 1.0 ||
        objectDepth <= 0.0 || objectDepth >= 1.0) {
      gl_FragColor = vec4(rgb, 1.0);
      gl_FragDepth = objectDepth;
      return;
    }

    // Test virtual object depth with environment depth.
    // If the virtual object is further away (occluded) output a transparent color so real scene content from PT layer is displayed.

    highp float occlusionFactor = 0.0;
    if (u_softOcclusion == 0 || u_occlusionSoftness <= 0.0) {
      occlusionFactor = step(depthViewEyeZ - u_occlusionDepthBias, objectDepth);
    } else {
      highp float occlusionSum = 0.0;
      highp float validCount = 0.0;
      highp vec2 baseUv = objectDepthCameraPositionHC;
      highp vec2 offsets[4];
      offsets[0] = vec2(-0.5, -0.5);
      offsets[1] = vec2(0.5, -0.5);
      offsets[2] = vec2(-0.5, 0.5);
      offsets[3] = vec2(0.5, 0.5);
      for (int i = 0; i < 4; ++i) {
        highp vec2 uv = baseUv + offsets[i] * u_environmentDepthTexelSize;
        highp float sampleDepth = texture(u_environmentDepthTexture, vec3(uv, VIEW_ID)).r;
        if (sampleDepth > 0.0 && sampleDepth < 1.0) {
          highp float edge0 = sampleDepth - u_occlusionDepthBias;
          highp float edge1 = edge0 + u_occlusionSoftness;
          occlusionSum += smoothstep(edge0, edge1, objectDepth);
          validCount += 1.0;
        }
      }
      occlusionFactor = (validCount > 0.0) ? (occlusionSum / validCount) : 0.0;
    }

    gl_FragColor.rgb = rgb;
    gl_FragColor.a = 1.0 - occlusionFactor;

    gl_FragDepth = objectDepth;
}
)glsl";
