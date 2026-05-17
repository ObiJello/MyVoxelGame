// File: shaders/portal_particle_vk.frag
// Vulkan version of PortalParticleSystem's fragment shader. Mirrors
// the OpenGL inline source: sRGB→linear conversion on the per-particle
// color, Source SDK's `sprite.rgb * particle.color * particle.alpha`
// formula with additive blend (set in the pipeline state, not the
// shader), procedural fallback disc when the Portal-extracted sprite
// PNGs failed to load. uHasSprite arrives via pc.uScalars.x — written
// from VKBackend's SetUniformInt("uHasSprite", 0|1).
#version 450

layout(set = 0, binding = 0) uniform sampler2D uSprite;

layout(push_constant) uniform PC {
    mat4  uMVP;
    vec2  uScreenSize;
    float uLineWidth;
    float uAlphaTest;
    vec4  uColor;
    vec4  uUVRange;
    vec4  uScalars;     // x = uHasSprite (0 or 1)
} pc;

layout(location = 0) in vec2 vUV;
layout(location = 1) in vec4 vColor;
layout(location = 0) out vec4 FragColor;

void main() {
    float lifeAlpha = vColor.a;
    bool hasSprite = pc.uScalars.x > 0.5;
    if (hasSprite) {
        vec4 s = texture(uSprite, vUV);
        float texLum = max(max(s.r, s.g), s.b);
        if (texLum < 0.005) discard;
        // sRGB→linear approximation on the per-particle color (vertex
        // attributes aren't sRGB-aware; matches GL fragment shader).
        vec3 particleColorLinear = pow(vColor.rgb, vec3(2.2));
        FragColor = vec4(s.rgb * particleColorLinear * lifeAlpha, 1.0);
    } else {
        float r = length(vUV - vec2(0.5)) * 2.0;
        float disc = 1.0 - smoothstep(0.55, 1.0, r);
        if (disc <= 0.0) discard;
        FragColor = vec4(vColor.rgb * disc * lifeAlpha, 1.0);
    }
}
