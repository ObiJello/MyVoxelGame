// File: shaders/crosshair_vk.frag (Vulkan version of crosshair GL shader)
#version 450

layout(location = 0) in vec2 TexCoord;

layout(set = 0, binding = 0) uniform sampler2D uTexture;

layout(push_constant) uniform PushConstants {
    mat4 uMVP;          // 64 bytes
    vec2 uScreenSize;   // 8 bytes
    float uLineWidth;   // 4 bytes
    float uAlphaTest;   // 4 bytes
} pc;

layout(location = 0) out vec4 FragColor;

void main() {
    vec4 texColor = texture(uTexture, TexCoord);
    if (texColor.a < 0.5) discard;
    // Force alpha to 1.0 — OneMinusDstColor blending zeroes alpha against
    // an opaque framebuffer (1 - 1 = 0), making the crosshair invisible.
    FragColor = vec4(texColor.rgb, 1.0);
}
