// File: shaders/entity_outline_blur_vk.frag (Vulkan twin of
// entity_outline_blur.frag — MC post/entity_outline_box_blur.fsh).
#version 450

layout(location = 0) in vec2 texCoord;
layout(location = 0) out vec4 fragColor;

layout(set = 0, binding = 0) uniform sampler2D uTex;

layout(push_constant) uniform PushConstants {
    mat4 uMVP;
    vec2 uInSize;       // 64 — the uScreenSize slot
    float uLineWidth;   // 72
    float uAlphaTest;   // 76
    vec4 uColor;        // 80
    vec2 uBlurDir;      // 96 — the uUVRange slot's first half (VKBackend routes uBlurDir)
} pc;

void main() {
    vec2 oneTexel = 1.0 / pc.uInSize;
    vec2 sampleStep = oneTexel * pc.uBlurDir;

    vec4 blurred = vec4(0.0);
    float radius = 2.0;
    for (float a = -radius + 0.5; a <= radius; a += 2.0) {
        blurred += texture(uTex, texCoord + sampleStep * a);
    }
    blurred += texture(uTex, texCoord + sampleStep * radius) / 2.0;
    fragColor = vec4((blurred / (radius + 0.5)).rgb, blurred.a);
}
