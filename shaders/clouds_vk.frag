// File: shaders/clouds_vk.frag
#version 450

layout(location = 0) in vec4 vColor;
layout(location = 1) in float vDist;
layout(location = 0) out vec4 FragColor;

layout(set = 0, binding = 0) uniform sampler2D uTexture; // unused (dummy white)

layout(push_constant) uniform PushConstants {
    mat4 uMVP;
    vec2 uScreenSize;
    float uLineWidth;
    float uAlphaTest;
    vec4 uColor;
} pc;

layout(std140, set = 1, binding = 0) uniform Common {
    mat4  uMVP_;
    mat4  uModel_;
    vec4  uPortalColor_;
    vec4  uColorDark_;
    vec4  uColorHot_;
    vec4  uKeyDir_;
    vec4  uTint_;
    vec4  uUVRange_;
    vec4  uScalarsA_;
    vec4  uScalarsB_;
    vec4  uScalarsC_;
    vec4  uScalarsD_;
    vec2  uScreenSize_;
    vec2  _pad_;
    vec4  uFogColor_;
    vec4  uFogEnv_;
    vec4  uCamPosBright_;
} U;

float linearFog(float d, float s, float e) {
    if (d <= s) return 0.0;
    if (d >= e) return 1.0;
    return (d - s) / (e - s);
}

void main() {
    // MC rendertype_clouds.fsh: fog fades ALPHA (not a color mix).
    vec4 color = vColor * pc.uColor;
    color.a *= 1.0 - linearFog(vDist, U.uFogEnv_.x, U.uFogEnv_.y);
    if (color.a <= 0.0) discard;
    FragColor = color;
}
