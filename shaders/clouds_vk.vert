// File: shaders/clouds_vk.vert
//
// Minecraft cloud pass. Companion to CloudRenderer.{hpp,cpp}. Uses the
// UBO-aware (portal) pipeline layout: uMVP via push constants, uModel and
// the environment fog fields via the Common UBO (set=1).
#version 450

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aUV;
layout(location = 2) in vec4 aColor;

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

layout(location = 0) out vec4 vColor;
layout(location = 1) out float vDist;

out gl_PerVertex { vec4 gl_Position; };

void main() {
    gl_Position = pc.uMVP * vec4(aPos, 1.0);
    vColor = aColor;
    vDist = length((U.uModel_ * vec4(aPos, 1.0)).xyz);
}
