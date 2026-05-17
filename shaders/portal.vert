// File: shaders/portal.vert
// OpenGL portal renderer vertex shader.
// Mirrors PortalRenderer::vertexShaderSource so both GL and VK can load
// from disk via CreateShaderFromFiles* (Vulkan path-rewrites to portal_vk.spv).
#version 330 core

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aUV;
layout(location = 2) in vec4 aColor;

uniform mat4  uMVP;
uniform float uPulse;
uniform float uTimeVS;
uniform float uOpenAmountVS;

out vec2 vUV;
out vec4 vNoiseUV;

void main() {
    vec3 pos = vec3(aPos.x * uPulse, aPos.y * uPulse, aPos.z);
    gl_Position = uMVP * vec4(pos, 1.0);

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
