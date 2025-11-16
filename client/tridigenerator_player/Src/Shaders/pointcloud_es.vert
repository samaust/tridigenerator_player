#version 300 es
precision highp float;

layout(location = 0) in vec3 inPosition; // from VBO: px,py,pz (p.z may be placeholder)
layout(location = 1) in vec2 inUV;

uniform mat4 u_model;
uniform mat4 u_view;
uniform mat4 u_proj;

uniform sampler2D uPosTex;    // if positions provided as texture
out vec2 vUV;
out vec3 vWorldPos;

void main() {
    vec3 pos = inPosition;
    // If you supply positions in a texture (posTex with RGBA32F), sample here:
    #ifdef USE_POS_TEX
    vec4 p = texture(uPosTex, inUV);
    pos = p.xyz;
    #endif

    vec4 world = u_model * vec4(pos, 1.0);
    vWorldPos = world.xyz;
    vUV = inUV;
    gl_Position = u_proj * u_view * world;
}
