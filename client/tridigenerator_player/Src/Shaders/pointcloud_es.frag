#version 300 es
precision highp float;

in vec2 vUV;
in vec3 vWorldPos;

uniform sampler2D uColorTex;   // RGB8
uniform sampler2D uClassTex;   // R8 normalized
uniform sampler2D uDepthTex;   // optional GPU depth (if available)
uniform vec3 uAmbient;

out vec4 fragColor;

void main() {
  vec3 color = texture(uColorTex, vUV).rgb;
  float cls = texture(uClassTex, vUV).r; // range 0..1

  // classification 7 mapped to 1.0 on server; adjust threshold accordingly
  if (cls > 0.85) discard;

  // ambient correction
  vec3 lit = color * uAmbient;

  // depth occlusion blending (if depth available)
  #ifdef USE_DEPTH_TEX
    float sceneDepth = texture(uDepthTex, gl_FragCoord.xy / vec2(float(textureSize(uDepthTex,0).x), float(textureSize(uDepthTex,0).y))).r;
    float meshDepth = gl_FragCoord.z;
    float alpha = 1.0;
    if (meshDepth > sceneDepth + 0.01) alpha = 0.25;
    fragColor = vec4(lit, alpha);
  #else
    fragColor = vec4(lit, 1.0);
  #endif
}
