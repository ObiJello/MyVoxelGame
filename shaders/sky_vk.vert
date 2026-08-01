// File: shaders/sky_vk.vert
//
// Minecraft sky pass (disc / sunrise fan / sun / moon / stars).
// Companion to SkyRenderer.{hpp,cpp}. Uses the UBO-aware (portal) pipeline
// layout: push constants carry uMVP/uColor, the Common UBO (set=1) carries
// uFogColor/uFogEnv appended at offset 304.
#version 450

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aUV;
layout(location = 2) in vec4 aColor;

layout(push_constant) uniform PushConstants {
    mat4 uMVP;          // 0
    vec2 uScreenSize;   // 64
    float uLineWidth;   // 72
    float uAlphaTest;   // 76
    vec4 uColor;        // 80
} pc;

layout(location = 0) out vec2 vUV;
layout(location = 1) out vec4 vColor;
layout(location = 2) out float vSph;
layout(location = 3) out float vCyl;

out gl_PerVertex { vec4 gl_Position; };

void main() {
    gl_Position = pc.uMVP * vec4(aPos, 1.0);
    vUV = aUV;
    vColor = aColor;
    // Fog distances from the raw buffer position (camera-centered sky).
    vSph = length(aPos);
    vCyl = max(length(aPos.xz), abs(aPos.y));
}
