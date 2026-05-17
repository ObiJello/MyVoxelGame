// File: shaders/viewmodel_skinned_vk.frag
// Vulkan viewmodel fragment shader. Same CommonUBO header as
// viewmodel_skinned_vk.vert. Texture binding=0 on the portal layout.
#version 450

layout(set = 0, binding = 0) uniform sampler2D uDiffuse;

layout(std140, set = 1, binding = 0) uniform Common {
    mat4  uMVP_;
    mat4  uModel_;
    vec4  uPortalColor_;
    vec4  uColorDark_;
    vec4  uColorHot_;
    vec4  uKeyDir_;         // xyz=dir, w=uKeyIntensity
    vec4  uTint_;
    vec4  uUVRange_;
    vec4  uScalarsA_;
    vec4  uScalarsB_;
    vec4  uScalarsC_;       // (uAmbient, uAlphaCutoff, uExposure, uHasBloom)
    vec4  uScalarsD_;
    vec2  uScreenSize_;
    vec2  _pad_;
} U;

#define uKeyDir         U.uKeyDir_.xyz
#define uKeyIntensity   U.uKeyDir_.w
#define uAmbient        U.uScalarsC_.x
#define uAlphaCutoff    U.uScalarsC_.y

layout(location = 0) in vec2 vUV;
layout(location = 1) in vec3 vNormalCam;
layout(location = 0) out vec4 FragColor;

void main() {
    vec4 t = texture(uDiffuse, vUV);
    if (uAlphaCutoff > 0.0 && t.a < uAlphaCutoff) discard;
    vec3  n     = normalize(vNormalCam);
    float ndl   = max(dot(n, -normalize(uKeyDir)), 0.0);
    float halfL = pow(ndl * 0.5 + 0.5, 2.0);
    float shade = clamp(uAmbient + uKeyIntensity * halfL, 0.0, 1.5);
    FragColor   = vec4(t.rgb * shade, t.a);
}
