// File: shaders/sky_vk.frag
#version 450

layout(location = 0) in vec2 vUV;
layout(location = 1) in vec4 vColor;
layout(location = 2) in float vSph;
layout(location = 3) in float vCyl;
layout(location = 0) out vec4 FragColor;

layout(set = 0, binding = 0) uniform sampler2D uTexture;

layout(push_constant) uniform PushConstants {
    mat4 uMVP;
    vec2 uScreenSize;
    float uLineWidth;
    float uAlphaTest;
    vec4 uColor;
} pc;

// Common UBO (portal pipeline layout, set=1). Only the appended environment
// fields are needed, but std140 offsets require declaring the full prefix.
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
    vec4  uFogColor_;      // 304 — rgb = fog color, a = fog color alpha
    vec4  uFogEnv_;        // 320 — (envStart, envEnd, rdStart, rdEnd)
    vec4  uCamPosBright_;  // 336 — xyz = uCameraPos, w = uSkyBrightness
} U;

float linearFog(float d, float s, float e) {
    if (d <= s) return 0.0;
    if (d >= e) return 1.0;
    return (d - s) / (e - s);
}

void main() {
    vec4 color = texture(uTexture, vUV) * vColor * pc.uColor;
    if (color.a == 0.0) discard;
    float fogValue = max(linearFog(vSph, U.uFogEnv_.x, U.uFogEnv_.y),
                         linearFog(vCyl, U.uFogEnv_.z, U.uFogEnv_.w));
    FragColor = vec4(mix(color.rgb, U.uFogColor_.rgb, fogValue * U.uFogColor_.a), color.a);
}
