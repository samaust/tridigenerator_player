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
	uniform highp mat4 u_depthViewMatrix[NUM_VIEWS];
	uniform highp mat4 u_depthProjectionMatrix[NUM_VIEWS];
	uniform highp sampler2DArray u_environmentDepthTexture;

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
    highp vec2 objectDepthCameraPositionHC = objectDepthCameraPosition.xy / objectDepthCameraPosition.w;
    objectDepthCameraPositionHC = objectDepthCameraPositionHC * 0.5f + 0.5f;

    // Sample from Environment Depth API texture
    highp vec3 depthViewCoord = vec3(objectDepthCameraPositionHC, VIEW_ID);
    highp float depthViewEyeZ = texture(u_environmentDepthTexture, depthViewCoord).r;

    // Get virtual object depth
    highp float objectDepth = objectDepthCameraPosition.z / objectDepthCameraPosition.w;
    objectDepth = objectDepth * 0.5f + 0.5f;

    // Test virtual object depth with environment depth.
    // If the virtual object is further away (occluded) output a transparent color so real scene content from PT layer is displayed.

    gl_FragColor.rgb = rgb;
    if (objectDepth < depthViewEyeZ) {
      gl_FragColor.a = 1.0; // fully opaque
    }
    else {
      gl_FragColor = vec4(0.0, 0.0, 0.0, 0.0); // invisible
    }

    gl_FragDepth = objectDepth;
}
)glsl";
