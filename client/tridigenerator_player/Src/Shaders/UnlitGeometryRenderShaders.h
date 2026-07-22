#pragma once

const char* UnlitGeometryVertexShaderSrc = R"glsl(
// Attributes
attribute highp vec4 Position;
attribute highp vec3 Normal;
attribute highp vec2 TexCoord;

uniform usampler2D u_texDepth;
uniform highp vec4 u_intrinsics; // fx, fy, cx, cy in pixels
uniform highp vec2 u_imageSize;
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

    highp vec2 pixel = TexCoord * u_imageSize;
    float x = (pixel.x - u_intrinsics.z) * z / u_intrinsics.x;
    float y = -(pixel.y - u_intrinsics.w) * z / u_intrinsics.y;

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
	uniform highp usampler2D u_texAlpha;
	uniform highp usampler2D u_texDepth;
	uniform highp int u_maskVisibility[256];
	uniform lowp int u_hasEnvironmentDepth;
	uniform highp vec4 u_occlusionParams; // soft-enabled, softness, bias, unused
	uniform highp mat4 u_depthViewMatrix[NUM_VIEWS];
	uniform highp mat4 u_depthProjectionMatrix[NUM_VIEWS];
	uniform highp sampler2DArray u_environmentDepthTexture;
	uniform highp vec2 u_environmentDepthTexelSize;
	uniform highp sampler3D u_lightField;
	uniform highp sampler2D u_datasetColorReference;
	uniform highp mat4 u_lightParams;
	uniform highp vec4 u_matchingLimits; // min tint, max tint, min exposure, max exposure

varying lowp vec2 oTexCoord;
varying lowp vec4 oColor;
varying highp vec4 worldPosition;

#ifndef YUV_FULL_RANGE
#define YUV_FULL_RANGE 0
#endif

vec3 yuv_to_rgb(float y, float u, float v) {
    float d = u - 0.5;
    float e = v - 0.5;
#if YUV_FULL_RANGE
    float r = y + 1.402 * e;
    float g = y - 0.344136 * d - 0.714136 * e;
    float b = y + 1.772 * d;
    return vec3(r, g, b);
#else
    float c = y - 0.0625;
    float r = 1.1643 * c + 1.5958 * e;
    float g = 1.1643 * c - 0.39173 * d - 0.81290 * e;
    float b = 1.1643 * c + 2.017 * d;
    return vec3(r, g, b);
#endif
}

vec3 srgb_to_linear(vec3 c) {
    c = clamp(c, 0.0, 1.0);
    return vec3(
        (c.r <= 0.04045) ? (c.r / 12.92) : pow((c.r + 0.055) / 1.055, 2.4),
        (c.g <= 0.04045) ? (c.g / 12.92) : pow((c.g + 0.055) / 1.055, 2.4),
        (c.b <= 0.04045) ? (c.b / 12.92) : pow((c.b + 0.055) / 1.055, 2.4)
    );
}

#define TransformDepthVertex(localPos) (u_depthProjectionMatrix[VIEW_ID] * ( u_depthViewMatrix[VIEW_ID] * localPos ))

void main()
{
    uint maskId = texture(u_texAlpha, oTexCoord).r;
    if (u_maskVisibility[int(maskId)] == 0 ||
        texture(u_texDepth, oTexCoord).r == uint(0)) discard;

	// Get color value from YUV textures
	float y = texture(u_texY, oTexCoord).r;
	float u = texture(u_texU, oTexCoord).r;
	float v = texture(u_texV, oTexCoord).r;
	vec3 rgb = srgb_to_linear(yuv_to_rgb(y, u, v));

	vec4 estimatedLight = u_lightParams[0];
	if (u_lightParams[1].w >= 2.0) {
	    highp vec3 gridUv = (worldPosition.xyz - u_lightParams[1].xyz) * u_lightParams[2].xyz;
	    if (all(greaterThanEqual(gridUv, vec3(0.0))) && all(lessThanEqual(gridUv, vec3(1.0)))) {
	        estimatedLight = texture(u_lightField, gridUv);
	    }
	}
	highp float matchAmount = clamp(u_lightParams[2].w * u_lightParams[3].x, 0.0, 1.0);
	highp vec4 datasetReference = texelFetch(u_datasetColorReference, ivec2(int(maskId), 0), 0);
	highp vec3 chromaGain = clamp(
	    estimatedLight.rgb / max(datasetReference.rgb, vec3(0.001)),
	    vec3(u_matchingLimits.x), vec3(u_matchingLimits.y));
	highp float luminanceGain = clamp(
	    estimatedLight.a / max(datasetReference.a, 0.001),
	    u_matchingLimits.z, u_matchingLimits.w);
	rgb *= mix(vec3(1.0), chromaGain * luminanceGain, matchAmount);

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
    if (u_occlusionParams.x < 0.5 || u_occlusionParams.y <= 0.0) {
      occlusionFactor = step(depthViewEyeZ - u_occlusionParams.z, objectDepth);
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
          highp float edge0 = sampleDepth - u_occlusionParams.z;
          highp float edge1 = edge0 + u_occlusionParams.y;
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
