// File: shaders/portal.vert
// OpenGL portal renderer vertex shader.
// Mirrors PortalRenderer::vertexShaderSource so both GL and VK can load
// from disk via CreateShaderFromFiles* (Vulkan path-rewrites to portal_vk.spv).
#version 330 core

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aUV;
layout(location = 2) in vec4 aColor;

uniform mat4  uMVP;
uniform mat4  uModel;          // surface → world; the fog overlay (uOutlineMode 3) fogs by world distance
uniform float uPulse;
uniform float uTimeVS;
uniform float uOpenAmountVS;
// World-space clip plane (xyz = normal, w = offset; zero = off). Inside a
// portal view this is the outer portal's inner clip plane, so a nested
// portal's surface — its stencil mark above all — is cut exactly where the
// far world is: a portal standing behind the far surface, between it and
// the far camera, must not punch a mask into the view (the mod's
// FrontClipping on ViewAreaRenderer).
uniform vec4  uPortalClipPlane;

out vec2 vUV;
out vec4 vNoiseUV;
out vec3 vWorldPos;

void main() {
    vec3 pos = vec3(aPos.x * uPulse, aPos.y * uPulse, aPos.z);
    gl_Position = uMVP * vec4(pos, 1.0);
    vWorldPos = (uModel * vec4(pos, 1.0)).xyz;
    gl_ClipDistance[0] = (any(notEqual(uPortalClipPlane.xyz, vec3(0.0))))
        ? dot(uPortalClipPlane.xyz, vWorldPos) + uPortalClipPlane.w
        : 1.0;

    const float kOuterBorder = 0.075;
    vUV = aUV * (1.0 + kOuterBorder) - vec2(kOuterBorder * 0.5);

    float openAmtVS = clamp(uOpenAmountVS + 0.001, 0.0, 1.0);
    const float kNoiseScale  = 0.3;
    const float kScrollRate  = 0.0275;
    float scroll = uTimeVS * kScrollRate;
    vec2 noiseBase = (aUV - 0.5) / openAmtVS + 0.5;
    vNoiseUV.xy = noiseBase * kNoiseScale + vec2( scroll, 0.0);
    vNoiseUV.zw = noiseBase * kNoiseScale - vec2( scroll, 0.0);
}
